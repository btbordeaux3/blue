#ifndef ML_MODEL_H
#define ML_MODEL_H

#include <Arduino.h>
#include "common.h"

/*
 * ml_model.h - On-device TinyML classifier for proactive repeater activation
 *
 * The base station learns, from the mobile unit's own reports, WHEN and
 * WHERE the link goes bad, then turns the repeater on BEFORE the signal
 * collapses.
 *
 * Features (7 total), built by ml_build_features():
 *   0..3  AP-signature bins: BSSIDs feature-hashed into 4 slots (which
 *        APs surround us => WHERE we are). A location that has historically
 *        been weak gets its signature learned as BAD.
 *   4     current top-3 RSSI (normalized): lets the model separate
 *        "signal is fine here normally" from dead-zone moments.
 *   5,6   sin/cos(2*pi*hour/24): cyclical time encoding => proven dead-zone
 *        TIMES (e.g. dinner crowd swallows the 2.4GHz band every evening).
 *
 * Label: 1 if the mobile actually measured a weak link that report, else 0.
 *
 * Learning is a single-layer perceptron (logistic regression) trained
 * online - one SGD step per new sample. Samples live in a RING BUFFER
 * (ML_MAX_SAMPLES) that EVICTS THE OLDEST at capacity; the model is refit
 * after eviction so stale signatures roll off instead of accumulating
 * bias. Everything persists to SPIFFS across reboots.
 *
 * Cold start: ml_model_predict() returns should_activate=false until the
 * model has ML_MIN_SAMPLES samples AND at least ML_MIN_POS weak ones -
 * with no history the base does nothing and the mobile's own RSSI logic
 * covers early life.
 */

#define ML_NUM_FEATURES     7
#define ML_SIG_BINS         4          /* features 0..3 */
#define ML_RSSI_FEATURE     4
#define ML_HOUR_SIN_FEATURE 5
#define ML_HOUR_COS_FEATURE 6

#define ML_MIN_SAMPLES      40         /* cold-start floor */
#define ML_MIN_POS          5          /* must have SEEN some weak zones */
#define ML_PREDICT_THRESHOLD     0.62f /* sigmoid() gate - act on it */
#define ML_DEACTIVATE_THRESHOLD  0.38f /* inverse - confident it's good */
#define ML_LEARNING_RATE    0.05f
#define ML_RETRAIN_EPOCHS   8
#define ML_EMA_ALPHA        0.1f   /* rf_map EMA smoothing (storage/display) */

struct ml_sample_t {
    float features[ML_NUM_FEATURES];
    float label;                       /* 1.0 = weak zone, 0.0 = good */
};

struct ml_model_t {
    float weights[ML_NUM_FEATURES];
    float bias;
    ml_sample_t samples[ML_MAX_SAMPLES];  /* learning ring, oldest evicted */
    uint16_t sample_count;
    uint16_t head;                        /* oldest live sample index */
    uint32_t pos_count;                   /* labeled weak */
    uint32_t neg_count;                   /* labeled good */
    uint8_t  dirty;                       /* set when ring changed */
    bool     trained;
};

struct ml_prediction_t {
    float score;          /* raw pre-activation */
    float probability;    /* sigmoid(score): P(weak zone) */
    float confidence;     /* 2*|p-0.5| in [0,1] */
    bool  should_activate;    /* trained && p >= threshold */
    bool  should_deactivate;  /* trained && p <= deactivate threshold */
    bool  available;          /* enough data to make any call */
};

/* Builds the feature vector from a mobile scan report + current clock. */
void ml_build_features(const uint32_t *bssid_hashes, const rssi_t *rssi_values,
                       uint8_t count, rssi_t rssi_avg, uint8_t hour,
                       float out[ML_NUM_FEATURES]);

void ml_model_init(ml_model_t *model);
void ml_model_reset(ml_model_t *model);
/* Append one labeled sample; evicts the oldest at capacity (and marks the
 * model dirty so a refit drops the influence of evicted data). */
void ml_model_add_sample(ml_model_t *model, const ml_sample_t *sample);
/* One SGD epoch over the whole live ring (keeps eviction honest). */
void ml_model_retrain(ml_model_t *model);
void ml_model_predict(const ml_model_t *model, const float features[ML_NUM_FEATURES],
                      ml_prediction_t *pred);
void ml_model_save(const ml_model_t *model);
bool ml_model_load(ml_model_t *model);

struct ml_stats_t {
    uint16_t samples;
    uint16_t pos;
    uint16_t neg;
    bool     trained;
};
void ml_model_get_stats(const ml_model_t *model, ml_stats_t *stats);

/* ml_task runtime facade - owned by the base station command loop. */
void ml_task_setup(void);
ml_model_t *ml_task_model(void);
bool ml_task_learn(const ml_sample_t *sample);   /* true => model refit happened */
bool ml_task_save(void);

void rf_map_init(void);
bool rf_map_add_entry(const rf_map_entry_t *entry);
uint16_t rf_map_get_entries_for_bssid(uint32_t bssid_hash, rf_map_entry_t *out, uint16_t max_out);
bool rf_map_update_ema(uint32_t bssid_hash, uint8_t hour, rssi_t new_rssi);
void rf_map_print_stats(void);
uint16_t rf_map_get_count(void);
uint16_t rf_map_get_weak_count(void);

#endif