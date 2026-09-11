/*
 * command_task.cpp - Base station decision engine (ML-driven)
 *
 * This is the "brain" of the base station. It consumes every fingerprint
 * report the mobile sends, and uses it as a LABELLED TRAINING SAMPLE:
 *
 *   features  <- AP-signature (where we are) + top RSSI + hour-of-day
 *   label     <- did the mobile actually measure a weak link right now?
 *
 * It feeds that into the on-device classifier (ml_task), then uses the
 * classifier's prediction to decide whether to flip the repeater on via a
 * command to the mobile. Two roles, cleanly separated so the dashboard can
 * show who acted:
 *
 *   MOBILE_LOW  - the mobile reacts to its own measured weak RSSI (reactive)
 *   BASE_ML     - the base predicts a known-bad zone/time AHEAD of the
 *                 degradation and commands the repeater on (proactive)
 *
 * The base never reactively chases low RSSI - that's the mobile's job - and
 * it only turns OFF what it turned ON (the mobile owns its own recovery).
 *
 * Interview talking points:
 *   - Online learning: inference and training share one forward pass
 *   - Cold-start gate: with <ML_MIN_SAMPLES weak samples the base emits no
 *     commands; the system "does nothing until it has enough data"
 *   - Confidence-gated actions avoid flapping near the decision boundary
 *   - State echo: decision pushes are driven by the mobile's REPORTED state,
 *     so the website shows ground truth, not the base's intent
 */

#include "command_task.h"
#include "espnow_comm.h"
#include "rf_fingerprint.h"
#include "ml_model.h"
#include "storage_task.h"
#include "push_task.h"
#include "common.h"

static TaskHandle_t cmd_task_handle = NULL;

static connection_status_t conn_status = LOST;
static uint32_t last_mobile_seen_ms = 0;
static uint8_t active_clients = 0;
static uint32_t cmd_sequence = 0;

/* What the BASE has commanded the mobile to do. */
static bool repeater_active = false;
static bool ml_controlled = false;      /* base may only undo what it did */

/* What the MOBILE reports about itself (ground truth for the dashboard). */
static bool mobile_repeater_on = false;
static uint8_t mobile_last_state = STATE_SLEEP;
static activation_source_t cur_source = SRC_NONE;
static float cur_confidence = 0.0f;

static rf_map_entry_t pending_map_entries[32];
static uint8_t pending_map_count = 0;

static void push_current_decision(void) {
    ml_stats_t st;
    ml_model_get_stats(ml_task_model(), &st);

    push_decision_t d;
    memset(&d, 0, sizeof(d));
    d.active = mobile_repeater_on;
    d.source = cur_source;
    d.confidence = cur_confidence;
    d.model_trained = st.trained;
    d.model_samples = st.samples;
    d.model_pos = st.pos;
    d.model_neg = st.neg;
    d.mobile_state = mobile_last_state;
    push_task_send_decision(&d);
}

static const char *src_name(activation_source_t s) {
    switch (s) {
        case SRC_BASE_ML:     return "base_ml";
        case SRC_MOBILE_RSSI: return "mobile_low";
        case SRC_BASE_MANUAL: return "base_manual";
        default:              return "none";
    }
}

static void on_command_received(const command_msg_t *cmd) {
    Serial.printf("[CMD] Received command: %u, seq: %lu\n", cmd->command, cmd->sequence_num);
}

static void on_report_received(const fingerprint_msg_t *report, const uint8_t *mac) {
    last_mobile_seen_ms = millis();
    if (conn_status != CONNECTED) active_clients = 1;
    conn_status = CONNECTED;
    mobile_last_state = report->local_state;

    Serial.printf("[REPORT] Mobile state=%d, RSSI avg=%d, battery=%d%%, src=%s\n",
                  report->local_state, report->local_rssi_avg, report->battery_pct,
                  src_name(report->activation_source));

    espnow_send_ack(MSG_FINGERPRINT, report->sequence_num, 0);

    rf_fingerprint_t fp;
    memset(&fp, 0, sizeof(fp));
    fp.count = report->scan_count;
    memcpy(fp.bssid_hashes, report->bssid_hashes, sizeof(fp.bssid_hashes));
    memcpy(fp.rssi_values, report->rssi_values, sizeof(fp.rssi_values));
    push_task_send_fingerprint(&fp);

    struct tm timeinfo;
    time_t now;
    time(&now);
    localtime_r(&now, &timeinfo);

    /* 1. Persist the raw AP/hour sample for the RF map (display/analytics). */
    if (pending_map_count < 32) {
        for (uint8_t i = 0; i < report->scan_count && i < TOP_N_RSSI; i++) {
            rf_map_entry_t entry;
            entry.bssid_hash = report->bssid_hashes[i];
            entry.hour_of_day = timeinfo.tm_hour;
            entry.day_of_week = timeinfo.tm_wday;
            entry.rssi_mean = report->rssi_values[i];
            entry.rssi_std = 0;
            entry.sample_count = 1;
            entry.is_weak = (report->rssi_values[i] < WEAK_RSSI_THRESHOLD);
            pending_map_entries[pending_map_count++] = entry;
        }
    }

    /* 2. LEARN from this report: features + ground-truth label. */
    float feats[ML_NUM_FEATURES];
    ml_build_features(report->bssid_hashes, report->rssi_values,
                      report->scan_count, report->local_rssi_avg,
                      (uint8_t)timeinfo.tm_hour, feats);

    ml_sample_t sample;
    memcpy(sample.features, feats, sizeof(feats));
    sample.label = (report->local_rssi_avg < WEAK_RSSI_THRESHOLD) ? 1.0f : 0.0f;
    if (ml_task_learn(&sample)) {
        Serial.println("[ML] Refit from live ring");
    }

    /* 3. Sync dashboard state - push EXACTLY on observed transitions. */
    bool mobile_on_now = (report->local_state == STATE_REPEATER);
    if (mobile_on_now && !mobile_repeater_on) {
        cur_source = report->activation_source;
        cur_confidence = (float)report->confidence_pct / 100.0f;
        mobile_repeater_on = true;
        Serial.printf("[STATE] Repeater UP  - source: %s (conf %.0f%%)\n",
                      src_name(cur_source), cur_confidence * 100.0f);
        push_current_decision();
    } else if (!mobile_on_now && mobile_repeater_on) {
        Serial.printf("[STATE] Repeater DOWN - last source: %s\n", src_name(cur_source));
        mobile_repeater_on = false;
        push_current_decision();
    }

    /* 4. PROACTIVE control: base acts only through the model. */
    ml_prediction_t pred;
    ml_model_predict(ml_task_model(), feats, &pred);

    if (pred.should_activate && !repeater_active && !ml_controlled &&
        conn_status == CONNECTED && !mobile_on_now) {
        command_msg_t cmd;
        cmd.msg_type = MSG_COMMAND;
        cmd.command = CMD_ACTIVATE_REPEATER;
        cmd.bridge_channel = WIFI_CHANNEL;
        cmd.bridge_timeout_ms = BRIDGE_SAFETY_TIMEOUT_MS;
        cmd.sequence_num = ++cmd_sequence;
        cmd.expected_ack = cmd.sequence_num;
        cmd.source = SRC_BASE_ML;
        cmd.confidence_pct = (uint8_t)(pred.confidence * 100.0f);
        espnow_send_command(&cmd);
        repeater_active = true;
        ml_controlled = true;
        Serial.printf("[CMD] ML ACTIVATE (p=%.2f conf=%.2f, signature learned bad)\n",
                      pred.probability, pred.confidence);
    } else if (repeater_active && ml_controlled && pred.should_deactivate &&
               report->local_rssi_avg >= STRONG_RSSI_THRESHOLD) {
        command_msg_t cmd;
        cmd.msg_type = MSG_COMMAND;
        cmd.command = CMD_DEACTIVATE;
        cmd.bridge_channel = 0;
        cmd.bridge_timeout_ms = 0;
        cmd.sequence_num = ++cmd_sequence;
        cmd.expected_ack = cmd.sequence_num;
        cmd.source = SRC_BASE_ML;
        cmd.confidence_pct = (uint8_t)((1.0f - pred.confidence) * 100.0f);
        espnow_send_command(&cmd);
        repeater_active = false;
        ml_controlled = false;
        Serial.printf("[CMD] ML DEACTIVATE (p=%.2f, zone recovered)\n", pred.probability);
    }
}

static void on_heartbeat_received(const heartbeat_msg_t *hb, const uint8_t *mac) {
    last_mobile_seen_ms = millis();
    if (conn_status != CONNECTED) active_clients = 1;
    conn_status = CONNECTED;
}

static void on_reconnect_received(const reconnect_msg_t *re, const uint8_t *mac) {
    Serial.printf("[RECONNECT] Mobile reconnected, reason: %u, buffered: %u\n",
                  re->reason, re->buffered_count);
    conn_status = CONNECTED;
    last_mobile_seen_ms = millis();

    if (re->buffered_count > 0) {
        command_msg_t cmd;
        cmd.msg_type = MSG_COMMAND;
        cmd.command = CMD_SEND_BUFFERED;
        cmd.bridge_channel = 0;
        cmd.bridge_timeout_ms = 0;
        cmd.sequence_num = ++cmd_sequence;
        cmd.expected_ack = 0;
        cmd.source = SRC_NONE;
        cmd.confidence_pct = 0;
        espnow_send_command(&cmd);
    }
}

static void check_connection_timeout(void) {
    if (last_mobile_seen_ms == 0) return;
    uint32_t elapsed = millis() - last_mobile_seen_ms;

    if (elapsed > CONNECTION_TIMEOUT_MS && conn_status != LOST) {
        conn_status = LOST;
        active_clients = 0;
        Serial.println("[CMD] Mobile LOST - connection timeout");
        Serial.printf("[CMD] Last seen: %lu ms ago\n", elapsed);
    }
}

static void check_repeater_timeout(void) {
    static uint32_t last_warn = 0;
    if (ml_controlled && mobile_repeater_on &&
        (millis() - last_mobile_seen_ms > BRIDGE_SAFETY_TIMEOUT_MS) &&
        (millis() - last_warn > 30000)) {
        last_warn = millis();
        Serial.println("[CMD] ML-controlled repeater exceeded safety window - "
                       "deactivating to avoid hidden-node risks");
        command_msg_t cmd;
        cmd.msg_type = MSG_COMMAND;
        cmd.command = CMD_DEACTIVATE;
        cmd.bridge_channel = 0;
        cmd.bridge_timeout_ms = 0;
        cmd.sequence_num = ++cmd_sequence;
        cmd.expected_ack = cmd.sequence_num;
        cmd.source = SRC_BASE_ML;
        cmd.confidence_pct = 0;
        espnow_send_command(&cmd);
        ml_controlled = false;
        repeater_active = false;
    }
}

static void flush_pending_map(void) {
    for (uint8_t i = 0; i < pending_map_count; i++) {
        rf_map_update_ema(pending_map_entries[i].bssid_hash,
                          pending_map_entries[i].hour_of_day,
                          pending_map_entries[i].rssi_mean);
        /* Keep the raw observation durable as well as updating the in-memory
         * aggregate. The storage task owns the flash write and applies its
         * bounded-file compaction policy. */
        storage_task_write_map_entry(&pending_map_entries[i]);
    }
    pending_map_count = 0;
    push_task_send_status(rf_map_get_count(), rf_map_get_weak_count());
}

static void command_task(void *param) {
    Serial.println("[CMD] Task started");

    espnow_set_report_callback(on_report_received);
    espnow_set_cmd_callback(on_command_received);
    espnow_set_heartbeat_callback(on_heartbeat_received);
    espnow_set_reconnect_callback(on_reconnect_received);

    uint32_t last_flush = millis();

    while (1) {
        check_connection_timeout();
        check_repeater_timeout();

        if (millis() - last_flush > 60000) {
            flush_pending_map();
            ml_task_save();
            last_flush = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void command_task_init(void) {
}

void command_task_start(void) {
    xTaskCreatePinnedToCore(command_task, "cmd", 8192, NULL, 5, &cmd_task_handle, 0);
}

void command_task_notify_scan_done(void) {
    if (cmd_task_handle) {
        xTaskNotifyGive(cmd_task_handle);
    }
}

void command_task_process_report(const fingerprint_msg_t *report, const uint8_t *mac) {
    on_report_received(report, mac);
}

connection_status_t command_task_get_connection_status(void) {
    return conn_status;
}

uint8_t command_task_get_active_clients(void) {
    return active_clients;
}