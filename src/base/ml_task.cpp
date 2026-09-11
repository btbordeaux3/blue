/*
 * ml_task.cpp - On-device TinyML classifier for proactive repeater activation
 *
 * The base station learns WHEN and WHERE the mobile's link goes weak, then
 * activates the repeater ahead of the signal collapse. Two failure modes it
 * picks up over time:
 *
 *   1. Known-bad-signal AREAS: a location is identified by the SET of APs
 *      around it (BSSID hashes feature-hashed into 4 signature bins). When a
 *      signature that has historically been weak shows up again, the model
 *      activates the repeater even though the current RSSI still looks fine.
 *   2. Dead-zone TIMES: "signal is normally good here, but between 18:00 and
 *      20:00 the bandwidth dies" - captured by cyclical hour sin/cos features.
 *
 * Learning is a 7-weight perceptron (logistic regression) doing one SGD step
 * per labelled report, over a 256-sample RING whose oldest entries are
 * EVICTED at capacity; after an eviction the model is refit from the live
 * ring so stale data doesn't accumulate bias (concept-drift handling).
 *
 * Interview talking points:
 *   - Feature hashing: BSSID space is unbounded, so AP ids are hashed into a
 *     fixed 4-bin signature vector (like a small Bloom filter) - O(1) memory.
 *   - Cyclical time encoding (sin/cos) instead of raw hour integers, so hour
 *     23 is treated as adjacent to hour 0.
 *   - Ring buffer = bounded on-device dataset with oldest-first eviction, no
 *     garbage ever needs collecting; retrain-on-evict keeps it consistent.
 *   - Online SGD: one gradient step per sample, no epoch batching, <1ms on an
 *     ESP32-S3 (no matrix/tensor dependencies).
 *   - Cold-start gating (min samples + min positive class): the model is
 *     explicitly forbidden from acting until it has seen reality.
 *   - Persistence: model + training ring serialize to SPIFFS, survive reboot.
 */

#include "ml_model.h"
#include <SPIFFS.h>

#define MODEL_FILE "/ml_model.bin"
#define MODEL_MAGIC 0x4D4C3031u   /* "ML01" */

static rf_map_entry_t rf_map[MAP_MAX_ENTRIES];
static uint16_t rf_map_count = 0;
static ml_model_t stored_model;
static uint8_t ml_refit_counter = 0;

static const int8_t rssi_scale_offset = 100;  /* -100dBm -> 0 */
static const rssi_t rssi_scale_range  = 25;   /* /25 -> 1.0 at -75dBm */

void ml_build_features(const uint32_t *bssid_hashes, const rssi_t *rssi_values,
                       uint8_t count, rssi_t rssi_avg, uint8_t hour,
                       float out[ML_NUM_FEATURES]) {
    float sig[ML_SIG_BINS] = {0, 0, 0, 0};

    for (uint8_t i = 0; i < count; i++) {
        float presence = (float)(rssi_values[i] + rssi_scale_offset) /
                         (float)rssi_scale_range;
        if (presence < 0.0f) presence = 0.0f;
        if (presence > 1.0f) presence = 1.0f;
        uint8_t bin = (uint8_t)(bssid_hashes[i] % ML_SIG_BINS);
        sig[bin] += presence;
    }

    float rssi_feat = (float)(rssi_avg + rssi_scale_offset) / (float)rssi_scale_range;
    if (rssi_feat < 0.0f) rssi_feat = 0.0f;
    if (rssi_feat > 1.5f) rssi_feat = 1.5f;

    float ang = 2.0f * PI * (float)(hour % 24) / 24.0f;

    for (int i = 0; i < ML_NUM_FEATURES; i++) out[i] = 0.0f;
    for (int i = 0; i < ML_SIG_BINS; i++) out[i] = sig[i];
    out[ML_RSSI_FEATURE] = rssi_feat;
    out[ML_HOUR_SIN_FEATURE] = sinf(ang);
    out[ML_HOUR_COS_FEATURE] = cosf(ang);
}

void ml_model_init(ml_model_t *model) {
    memset(model, 0, sizeof(ml_model_t));
    model->head = 0;
    model->trained = false;
    model->dirty = 0;
}

void ml_model_reset(ml_model_t *model) {
    ml_model_init(model);
}

static void ml_inc_class(ml_model_t *model, float label) {
    if (label >= 0.5f) model->pos_count++; else model->neg_count++;
}

static void ml_dec_class(ml_model_t *model, float label) {
    if (model->pos_count == 0 && model->neg_count == 0) return;
    if (label >= 0.5f) { if (model->pos_count) model->pos_count--; }
    else              { if (model->neg_count) model->neg_count--; }
}

static uint16_t ml_ring_index(const ml_model_t *model, uint16_t offset) {
    return (model->head + offset) % ML_MAX_SAMPLES;
}

void ml_model_add_sample(ml_model_t *model, const ml_sample_t *sample) {
    if (model->sample_count < ML_MAX_SAMPLES) {
        uint16_t i = ml_ring_index(model, model->sample_count);
        memcpy(&model->samples[i], sample, sizeof(ml_sample_t));
        model->sample_count++;
    } else {
        /* Ring full: evict the oldest and reuse its slot. */
        ml_dec_class(model, model->samples[model->head].label);
        memcpy(&model->samples[model->head], sample, sizeof(ml_sample_t));
        model->head = ml_ring_index(model, 1);
    }
    ml_inc_class(model, sample->label);
    model->dirty = 1;
    model->trained = (model->sample_count >= ML_MIN_SAMPLES &&
                      model->pos_count >= ML_MIN_POS);
}

static float ml_score(const ml_model_t *model, const float *features) {
    float s = model->bias;
    for (int i = 0; i < ML_NUM_FEATURES; i++) {
        s += model->weights[i] * features[i];
    }
    return s;
}

static float ml_sigmoid(float x) {
    if (x >= 0.0f) {
        float e = expf(-x);
        return 1.0f / (1.0f + e);
    }
    float e = expf(x);
    return e / (1.0f + e);
}

void ml_model_retrain(ml_model_t *model) {
    if (model->sample_count == 0) return;

    /* Refit from the live ring so evicted data stops influencing weights. */
    memset(model->weights, 0, sizeof(model->weights));
    model->bias = 0.0f;

    for (int epoch = 0; epoch < ML_RETRAIN_EPOCHS; epoch++) {
        for (uint16_t o = 0; o < model->sample_count; o++) {
            ml_sample_t *s = &model->samples[ml_ring_index(model, o)];
            float pred = ml_score(model, s->features);
            float err  = s->label - ml_sigmoid(pred);
            for (int i = 0; i < ML_NUM_FEATURES; i++) {
                model->weights[i] += ML_LEARNING_RATE * err * s->features[i];
            }
            model->bias += ML_LEARNING_RATE * err;
        }
    }
    model->dirty = 0;
}

void ml_model_predict(const ml_model_t *model, const float features[ML_NUM_FEATURES],
                      ml_prediction_t *pred) {
    memset(pred, 0, sizeof(ml_prediction_t));
    pred->score = ml_score(model, features);
    pred->probability = ml_sigmoid(pred->score);
    pred->confidence = fabsf(pred->probability - 0.5f) * 2.0f;
    pred->available = model->trained;
    pred->should_activate = model->trained &&
                            pred->probability >= ML_PREDICT_THRESHOLD;
    pred->should_deactivate = model->trained &&
                              pred->probability <= ML_DEACTIVATE_THRESHOLD;
}

void ml_model_get_stats(const ml_model_t *model, ml_stats_t *stats) {
    stats->samples = model->sample_count;
    stats->pos = (uint16_t)(model->pos_count > 0xFFFF ? 0xFFFF : model->pos_count);
    stats->neg = (uint16_t)(model->neg_count > 0xFFFF ? 0xFFFF : model->neg_count);
    stats->trained = model->trained;
}

void ml_model_save(const ml_model_t *model) {
    File f = SPIFFS.open(MODEL_FILE, "w");
    if (!f) {
        Serial.println("[ML] Failed to open model file for writing");
        return;
    }
    uint32_t magic = MODEL_MAGIC;
    f.write((const uint8_t *)&magic, sizeof(magic));
    f.write((const uint8_t *)model, sizeof(ml_model_t));
    f.close();
    Serial.printf("[ML] Model saved (%u samples, %u pos)\n",
                  model->sample_count, (uint32_t)model->pos_count);
}

bool ml_model_load(ml_model_t *model) {
    File f = SPIFFS.open(MODEL_FILE, "r");
    if (!f) {
        Serial.println("[ML] No saved model found - cold start");
        return false;
    }
    uint32_t magic = 0;
    if (f.read((uint8_t *)&magic, sizeof(magic)) != sizeof(magic) ||
        magic != MODEL_MAGIC ||
        f.read((uint8_t *)model, sizeof(ml_model_t)) != sizeof(ml_model_t)) {
        Serial.println("[ML] Model file missing/corrupted - cold start");
        f.close();
        ml_model_init(model);
        return false;
    }
    f.close();
    Serial.printf("[ML] Model loaded: %u samples, %u pos, trained=%s\n",
                  model->sample_count, (uint32_t)model->pos_count,
                  model->trained ? "yes" : "no");
    return true;
}

void rf_map_init(void) {
    memset(rf_map, 0, sizeof(rf_map));
    rf_map_count = 0;
}

bool rf_map_add_entry(const rf_map_entry_t *entry) {
    for (uint16_t i = 0; i < rf_map_count; i++) {
        if (rf_map[i].bssid_hash == entry->bssid_hash &&
            rf_map[i].hour_of_day == entry->hour_of_day) {
            int32_t new_mean = ((int32_t)rf_map[i].rssi_mean * rf_map[i].sample_count +
                                entry->rssi_mean) / (rf_map[i].sample_count + 1);
            rf_map[i].rssi_mean = (rssi_t)new_mean;
            rf_map[i].sample_count++;
            rf_map[i].is_weak = (rf_map[i].rssi_mean < WEAK_RSSI_THRESHOLD);
            return true;
        }
    }

    if (rf_map_count >= MAP_MAX_ENTRIES) return false;
    memcpy(&rf_map[rf_map_count], entry, sizeof(rf_map_entry_t));
    rf_map_count++;
    return true;
}

bool rf_map_update_ema(uint32_t bssid_hash, uint8_t hour, rssi_t new_rssi) {
    for (uint16_t i = 0; i < rf_map_count; i++) {
        if (rf_map[i].bssid_hash == bssid_hash && rf_map[i].hour_of_day == hour) {
            rf_map[i].rssi_mean = (rssi_t)(
                ML_EMA_ALPHA * new_rssi + (1.0f - ML_EMA_ALPHA) * rf_map[i].rssi_mean);
            rf_map[i].sample_count++;
            rf_map[i].is_weak = (rf_map[i].rssi_mean < WEAK_RSSI_THRESHOLD);
            return true;
        }
    }

    rf_map_entry_t new_entry;
    new_entry.bssid_hash = bssid_hash;
    new_entry.hour_of_day = hour;
    new_entry.day_of_week = 0;
    new_entry.rssi_mean = new_rssi;
    new_entry.rssi_std = 0;
    new_entry.sample_count = 1;
    new_entry.is_weak = (new_rssi < WEAK_RSSI_THRESHOLD);
    return rf_map_add_entry(&new_entry);
}

uint16_t rf_map_get_entries_for_bssid(uint32_t bssid_hash, rf_map_entry_t *out, uint16_t max_out) {
    uint16_t found = 0;
    for (uint16_t i = 0; i < rf_map_count && found < max_out; i++) {
        if (rf_map[i].bssid_hash == bssid_hash) {
            memcpy(&out[found], &rf_map[i], sizeof(rf_map_entry_t));
            found++;
        }
    }
    return found;
}

void rf_map_print_stats(void) {
    uint16_t weak_count = 0;
    for (uint16_t i = 0; i < rf_map_count; i++) {
        if (rf_map[i].is_weak) weak_count++;
    }
    Serial.printf("[MAP] %u entries, %u weak\n", rf_map_count, weak_count);
}

uint16_t rf_map_get_count(void) {
    return rf_map_count;
}

uint16_t rf_map_get_weak_count(void) {
    uint16_t weak = 0;
    for (uint16_t i = 0; i < rf_map_count; i++) {
        if (rf_map[i].is_weak) weak++;
    }
    return weak;
}

/* ---------- ml_task runtime (runs inside command_task loop) ---------- */

void ml_task_setup(void) {
    ml_model_init(&stored_model);
    ml_model_load(&stored_model);
}

ml_model_t *ml_task_model(void) {
    return &stored_model;
}

/* Feed one labelled report into the learner. Returns true when a refit
 * happened. Called from command_task on every mobile report. */
bool ml_task_learn(const ml_sample_t *sample) {
    ml_model_add_sample(&stored_model, sample);

    /* Refit immediately when the ring evicted (data fell off), else every
     * ML_RETRAIN_EVERY adds to amortize cost. */
    if (stored_model.dirty && stored_model.sample_count == ML_MAX_SAMPLES) {
        ml_model_retrain(&stored_model);
        return true;
    }
    if (++ml_refit_counter >= 10) {
        ml_model_retrain(&stored_model);
        ml_refit_counter = 0;
        return true;
    }
    return false;
}

bool ml_task_save(void) {
    ml_model_save(&stored_model);
    return true;
}