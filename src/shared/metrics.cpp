/*
 * metrics.cpp - Real-time system metrics for BLUE
 *
 * Implements Welford's online algorithm for numerically stable variance,
 * a lock-free ring buffer for event logging, operation timing with WCET
 * tracking, and FreeRTOS task health monitoring.
 *
 * Design decisions:
 *   - Fixed-size arrays (no malloc) for deterministic memory usage
 *   - All timestamps from micros()/millis() for hardware-backed timing
 *   - Stack watermarks read via uxTaskGetStackHighWaterMark() (FreeRTOS API)
 *   - Heap tracking via esp_get_free_heap_size() (ESP-IDF API)
 */

#include "metrics.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <math.h>

/* ---- Event Ring Buffer ---- */
static metrics_event_t event_log[METRICS_EVENT_LOG_SIZE];
static volatile uint32_t event_head = 0;   /* next write position */
static volatile uint32_t event_count = 0;  /* total events written */
static volatile uint32_t event_dropped = 0;

/* ---- Welford Statistics ---- */
static welford_state_t wifi_connect_time;
static welford_state_t scan_time;
static welford_state_t espnow_tx_time;
static welford_state_t rssi_stats;
static welford_state_t scan_ap_count;

/* ---- Operation Trackers ---- */
static op_tracker_t ops[METRICS_MAX_TRACKED_OPS];

/* ---- Cumulative Counters ---- */
static uint32_t wifi_connect_count = 0;
static uint32_t wifi_connect_fail_count = 0;
static uint32_t wifi_connected_ms = 0;
static uint32_t wifi_disconnected_ms = 0;
static uint32_t espnow_tx_count = 0;
static uint32_t espnow_tx_fail_count = 0;
static uint32_t espnow_rx_count = 0;
static uint32_t sleep_count = 0;
static uint32_t sleep_total_ms = 0;
static uint32_t repeater_active_count = 0;
static uint32_t repeater_total_ms = 0;
static uint32_t min_free_heap = 0xFFFFFFFF;

/* ---- Activation Latency ---- */
static welford_state_t activation_latency;
static uint32_t weak_signal_timestamp = 0;
static uint32_t activation_deadline_ms = 5000;
static uint32_t deadline_met_count = 0;
static uint32_t deadline_missed_count = 0;

/* ---- Timestamps ---- */
static uint32_t wifi_state_changed_at = 0;
static uint32_t repeater_activated_at = 0;
static uint32_t sleep_entered_at = 0;
static TaskHandle_t pm_task_handle = NULL;

/* Registered task handles for stack high-water marks. The mobile registers
 * pm/scan/report/repeater; the base registers its scan task. Slots that a
 * node never runs stay NULL and report 0 ("task never ran"). */
static TaskHandle_t task_handles[METRICS_TASK_SLOTS] = { NULL, NULL, NULL, NULL };

/* ======================================================================
 * Welford's Online Algorithm
 *
 * For interview: this computes mean and variance in a single pass with
 * O(1) memory. Unlike the two-pass algorithm (compute mean, then compute
 * sum of squared differences), this is numerically stable because it
 * never accumulates large intermediate sums. Each new sample x updates:
 *
 *   count += 1
 *   delta = x - mean
 *   mean += delta / count
 *   delta2 = x - mean
 *   m2 += delta * delta2
 *
 * After n samples:
 *   variance = m2 / (count - 1)     if count > 1
 *   stddev   = sqrt(variance)
 * ====================================================================== */
static void welford_update(welford_state_t *s, int32_t value) {
    s->count++;
    if (s->count == 1) {
        s->mean = (float)value;
        s->m2 = 0.0f;
        s->min = value;
        s->max = value;
    } else {
        float delta = (float)value - s->mean;
        s->mean += delta / (float)s->count;
        float delta2 = (float)value - s->mean;
        s->m2 += delta * delta2;
        if (value < s->min) s->min = value;
        if (value > s->max) s->max = value;
    }
}

static float welford_variance(const welford_state_t *s) {
    if (s->count < 2) return 0.0f;
    return s->m2 / (float)(s->count - 1);
}

static float welford_stddev(const welford_state_t *s) {
    return sqrtf(welford_variance(s));
}

/* ======================================================================
 * Ring Buffer Event Log
 *
 * Lock-free single-producer single-consumer ring buffer. The power manager
 * task writes events; the push task reads them for API upload. The head
 * pointer is only written by the producer. Overflow is tracked by counting
 * total events vs buffer size.
 * ====================================================================== */
static void ring_push(uint8_t type, uint32_t value, uint8_t extra) {
    uint32_t pos = event_head % METRICS_EVENT_LOG_SIZE;
    event_log[pos].timestamp_ms = millis();
    event_log[pos].type = type;
    event_log[pos].value = value;
    event_log[pos].extra = extra;

    event_head++;
    event_count++;

    if (event_count > METRICS_EVENT_LOG_SIZE &&
        (event_count - METRICS_EVENT_LOG_SIZE) < event_head) {
        event_dropped++;
    }
}

/* ======================================================================
 * Operation Timing with WCET Tracking
 *
 * For interview: WCET (Worst-Case Execution Time) is the single most
 * important metric in real-time systems. It determines the minimum
 * scheduling period needed to guarantee all tasks meet their deadlines.
 *
 * We track: count, failures, total time, and the worst single execution.
 * The WCET is never reset -- it monotonically increases to capture the
 * true worst case over the device's entire lifetime.
 * ====================================================================== */
static uint32_t next_op_handle = 0;

uint32_t metrics_begin_op(uint8_t op) {
    if (op >= METRICS_MAX_TRACKED_OPS) return 0xFFFFFFFF;
    uint32_t handle = next_op_handle++;
    ops[op].last_start_us = micros();
    ops[op].active = true;
    return (handle << 8) | op;
}

void metrics_end_op(uint32_t handle, bool success) {
    uint8_t op = handle & 0xFF;
    if (op >= METRICS_MAX_TRACKED_OPS) return;
    if (!ops[op].active) return;

    uint32_t elapsed_us = micros() - ops[op].last_start_us;
    ops[op].active = false;
    ops[op].count++;
    ops[op].total_time_us += elapsed_us;
    if (!success) ops[op].fail_count++;
    if (elapsed_us > ops[op].wcet_us) {
        ops[op].wcet_us = elapsed_us;
    }
}

/* ======================================================================
 * Public API
 * ====================================================================== */

void metrics_init(void) {
    memset(event_log, 0, sizeof(event_log));
    memset(&wifi_connect_time, 0, sizeof(welford_state_t));
    memset(&scan_time, 0, sizeof(welford_state_t));
    memset(&espnow_tx_time, 0, sizeof(welford_state_t));
    memset(&rssi_stats, 0, sizeof(welford_state_t));
    memset(&scan_ap_count, 0, sizeof(welford_state_t));
    memset(&activation_latency, 0, sizeof(welford_state_t));
    memset(ops, 0, sizeof(ops));
    event_head = 0;
    event_count = 0;
    event_dropped = 0;
    wifi_state_changed_at = millis();
    min_free_heap = esp_get_free_heap_size();
    Serial.println("[METRICS] Initialized");
}

/* ---- Event logging ---- */

void metrics_log_event(uint8_t type, uint32_t value, uint8_t extra) {
    ring_push(type, value, extra);
}

uint32_t metrics_get_events(metrics_event_t *out, uint32_t max_count) {
    uint32_t available = event_count < METRICS_EVENT_LOG_SIZE
                         ? event_count : METRICS_EVENT_LOG_SIZE;
    uint32_t to_copy = available < max_count ? available : max_count;
    uint32_t start = (event_count > METRICS_EVENT_LOG_SIZE)
                     ? (event_head - METRICS_EVENT_LOG_SIZE)
                     : 0;

    for (uint32_t i = 0; i < to_copy; i++) {
        uint32_t idx = (start + i) % METRICS_EVENT_LOG_SIZE;
        memcpy(&out[i], &event_log[idx], sizeof(metrics_event_t));
    }
    return to_copy;
}

uint32_t metrics_get_event_count(void) { return event_count; }
uint32_t metrics_get_events_dropped(void) { return event_dropped; }

/* ---- On-device statistics ---- */

void metrics_record_rssi(int32_t rssi) {
    welford_update(&rssi_stats, rssi);
}

void metrics_record_scan_ap_count(uint8_t count) {
    welford_update(&scan_ap_count, (int32_t)count);
}

void metrics_record_wifi_connect_time(uint32_t ms) {
    welford_update(&wifi_connect_time, (int32_t)ms);
}

void metrics_record_scan_time(uint32_t ms) {
    welford_update(&scan_time, (int32_t)ms);
}

void metrics_record_espnow_tx_time(uint32_t us) {
    welford_update(&espnow_tx_time, (int32_t)us);
}

/* ---- State tracking ---- */

void metrics_on_wifi_connect(void) {
    uint32_t now = millis();
    wifi_disconnected_ms += (now - wifi_state_changed_at);
    wifi_state_changed_at = now;
    wifi_connect_count++;
    metrics_log_event(EVT_WIFI_CONNECT, wifi_connect_count, 0);
}

void metrics_on_wifi_disconnect(void) {
    uint32_t now = millis();
    wifi_connected_ms += (now - wifi_state_changed_at);
    wifi_state_changed_at = now;
    wifi_connect_fail_count++;
    metrics_log_event(EVT_WIFI_DISCONNECT, wifi_connect_fail_count, 0);
}

void metrics_on_sleep_enter(void) {
    sleep_count++;
    sleep_entered_at = millis();
    metrics_log_event(EVT_SLEEP_ENTER, sleep_count, 0);
}

void metrics_on_sleep_wake(void) {
    sleep_total_ms += (millis() - sleep_entered_at);
    metrics_log_event(EVT_SLEEP_WAKE, millis() - sleep_entered_at, 0);
}

void metrics_on_repeater_activate(void) {
    repeater_active_count++;
    repeater_activated_at = millis();
    metrics_log_event(EVT_REPEATER_ACTIVATE, repeater_active_count, 0);
}

void metrics_on_repeater_deactivate(void) {
    repeater_total_ms += (millis() - repeater_activated_at);
    metrics_log_event(EVT_REPEATER_DEACTIVATE, millis() - repeater_activated_at, 0);
}

/* ---- Activation latency ---- */

void metrics_set_weak_signal_time(uint32_t timestamp_ms) {
    weak_signal_timestamp = timestamp_ms;
}

void metrics_record_activation_latency(uint32_t started_ms) {
    if (weak_signal_timestamp == 0) return;

    uint32_t latency = started_ms - weak_signal_timestamp;
    welford_update(&activation_latency, (int32_t)latency);
    metrics_log_event(EVT_ACTIVATION_LATENCY, latency, 0);

    if (latency <= activation_deadline_ms) {
        deadline_met_count++;
        metrics_log_event(EVT_DEADLINE_MET, latency, 0);
    } else {
        deadline_missed_count++;
        metrics_log_event(EVT_DEADLINE_MISSED, latency, 0);
    }
    weak_signal_timestamp = 0;
}

/* ---- Health monitoring ---- */

void metrics_set_pm_handle(TaskHandle_t handle) {
    metrics_register_task(METRICS_TASK_PM, handle);
}

void metrics_register_task(uint8_t slot, TaskHandle_t handle) {
    if (slot < METRICS_TASK_SLOTS) {
        task_handles[slot] = handle;
        if (slot == METRICS_TASK_PM) pm_task_handle = handle;
    }
}

void metrics_check_task_health(void) {
    uint32_t heap = esp_get_free_heap_size();
    if (heap < min_free_heap) min_free_heap = heap;

    if (heap < 8192) {
        metrics_log_event(EVT_HEAP_LOW, heap, 0);
    }

    if (pm_task_handle) {
        uint16_t pm_stack = uxTaskGetStackHighWaterMark(pm_task_handle);
        if (pm_stack < 512) {
            metrics_log_event(EVT_STACK_WARNING, pm_stack, 0);
        }
    }
}

/* ---- Snapshot for API ---- */

void metrics_get_snapshot(metrics_snapshot_t *out) {
    memset(out, 0, sizeof(metrics_snapshot_t));

    out->wifi_connect_count = wifi_connect_count;
    out->wifi_connect_fail_count = wifi_connect_fail_count;
    out->wifi_connect_time_ms_mean = wifi_connect_time.mean;
    out->wifi_connect_time_ms_p99 = (float)wifi_connect_time.max;

    out->scan_count = scan_time.count;
    out->scan_time_ms_mean = scan_time.mean;
    out->scan_time_ms_p99 = (float)scan_time.max;
    out->scan_ap_count_mean = (int32_t)(scan_ap_count.mean + 0.5f);

    out->espnow_tx_count = espnow_tx_count;
    out->espnow_tx_fail_count = espnow_tx_fail_count;
    out->espnow_rx_count = espnow_rx_count;
    out->espnow_tx_time_ms_mean = espnow_tx_time.mean / 1000.0f;

    out->wifi_connected_ms = wifi_connected_ms;
    out->wifi_disconnected_ms = wifi_disconnected_ms;
    uint32_t total_wifi = wifi_connected_ms + wifi_disconnected_ms;
    out->wifi_uptime_pct = total_wifi > 0
        ? (100.0f * (float)wifi_connected_ms / (float)total_wifi)
        : 0.0f;

    out->sleep_count = sleep_count;
    out->sleep_total_ms = sleep_total_ms;
    out->repeater_active_count = repeater_active_count;
    out->repeater_total_ms = repeater_total_ms;

    out->activation_latency_ms_mean = activation_latency.mean;
    out->activation_latency_ms_p99 = (float)activation_latency.max;
    out->activation_deadline_ms = activation_deadline_ms;
    out->deadline_met_count = deadline_met_count;
    out->deadline_missed_count = deadline_missed_count;

    out->free_heap_bytes = esp_get_free_heap_size();
    out->min_free_heap_bytes = min_free_heap;

    if (task_handles[METRICS_TASK_PM]) {
        out->pm_stack_remaining = uxTaskGetStackHighWaterMark(task_handles[METRICS_TASK_PM]);
    }
    if (task_handles[METRICS_TASK_SCAN]) {
        out->scan_stack_remaining = uxTaskGetStackHighWaterMark(task_handles[METRICS_TASK_SCAN]);
    }
    if (task_handles[METRICS_TASK_REPORT]) {
        out->report_stack_remaining = uxTaskGetStackHighWaterMark(task_handles[METRICS_TASK_REPORT]);
    }
    if (task_handles[METRICS_TASK_REPEATER]) {
        out->repeater_stack_remaining = uxTaskGetStackHighWaterMark(task_handles[METRICS_TASK_REPEATER]);
    }

    out->rssi_mean = rssi_stats.mean;
    out->rssi_stddev = welford_stddev(&rssi_stats);
    out->rssi_min = rssi_stats.min;
    out->rssi_max = rssi_stats.max;

    out->events_logged = event_count;
    out->events_dropped = event_dropped;
}

/* ---- JSON serialization ---- */

int metrics_snapshot_to_json(const metrics_snapshot_t *s, char *buf, size_t buf_len) {
    /* A fresh boot has 0 samples; mean/uptime/p99 can be NaN or +/-Inf,
     * which JSON.parse rejects ("No number after minus sign" for -nan).
     * Sanitize every float to 0 before formatting. */
    float wifi_mean = isfinite(s->wifi_connect_time_ms_mean) ? s->wifi_connect_time_ms_mean : 0.0f;
    float wifi_p99   = isfinite(s->wifi_connect_time_ms_p99)   ? s->wifi_connect_time_ms_p99   : 0.0f;
    float wifi_upt   = isfinite(s->wifi_uptime_pct)            ? s->wifi_uptime_pct            : 0.0f;
    float scan_mean  = isfinite(s->scan_time_ms_mean)           ? s->scan_time_ms_mean          : 0.0f;
    float scan_p99   = isfinite(s->scan_time_ms_p99)            ? s->scan_time_ms_p99           : 0.0f;
    float ap_mean    = isfinite(s->scan_ap_count_mean)          ? s->scan_ap_count_mean         : 0.0f;
    float espnow_us   = isfinite(s->espnow_tx_time_ms_mean)     ? s->espnow_tx_time_ms_mean     : 0.0f;
    float act_mean   = isfinite(s->activation_latency_ms_mean)  ? s->activation_latency_ms_mean : 0.0f;
    float act_p99    = isfinite(s->activation_latency_ms_p99)   ? s->activation_latency_ms_p99  : 0.0f;
    float rssi_mean  = isfinite(s->rssi_mean)                   ? s->rssi_mean                 : 0.0f;
    float rssi_std   = isfinite(s->rssi_stddev)                 ? s->rssi_stddev               : 0.0f;

    return snprintf(buf, buf_len,
        "{"
        "\"uptime_ms\":%lu,"
        "\"stack\":{"
            "\"pm\":%u,\"scan\":%u,\"report\":%u,\"repeater\":%u"
        "},"
        "\"wifi\":{"
            "\"connects\":%lu,\"connect_fails\":%lu,"
            "\"connect_ms_avg\":%.1f,\"connect_ms_worst\":%.0f,"
            "\"uptime_pct\":%.1f,"
            "\"connected_ms\":%lu,\"disconnected_ms\":%lu"
        "},"
        "\"scan\":{"
            "\"count\":%lu,\"time_ms_avg\":%.1f,\"time_ms_worst\":%.0f,"
            "\"ap_count_avg\":%.0f"
        "},"
        "\"espnow\":{"
            "\"tx\":%lu,\"tx_fail\":%lu,\"rx\":%lu,"
            "\"tx_us_avg\":%.0f"
        "},"
        "\"power\":{"
            "\"sleeps\":%lu,\"sleep_ms\":%lu,"
            "\"repeater_activations\":%lu,\"repeater_ms\":%lu"
        "},"
        "\"latency\":{"
            "\"activation_ms_avg\":%.1f,\"activation_ms_worst\":%.0f,"
            "\"deadline_ms\":%lu,\"deadline_met\":%lu,\"deadline_missed\":%lu"
        "},"
        "\"health\":{"
            "\"heap_free\":%lu,\"heap_min\":%lu,"
            "\"pm_stack\":%u"
        "},"
        "\"rssi\":{"
            "\"mean\":%.1f,\"stddev\":%.1f,\"min\":%d,\"max\":%d"
        "},"
        "\"events\":{"
            "\"logged\":%lu,\"dropped\":%lu"
        "}"
        "}",
        (unsigned long)(esp_timer_get_time() / 1000),
        s->pm_stack_remaining, s->scan_stack_remaining,
        s->report_stack_remaining, s->repeater_stack_remaining,
        s->wifi_connect_count, s->wifi_connect_fail_count,
        wifi_mean, wifi_p99,
        wifi_upt,
        s->wifi_connected_ms, s->wifi_disconnected_ms,
        s->scan_count, scan_mean, scan_p99,
        ap_mean,
        s->espnow_tx_count, s->espnow_tx_fail_count, s->espnow_rx_count,
        espnow_us,
        s->sleep_count, s->sleep_total_ms,
        s->repeater_active_count, s->repeater_total_ms,
        act_mean, act_p99,
        s->activation_deadline_ms, s->deadline_met_count, s->deadline_missed_count,
        s->free_heap_bytes, s->min_free_heap_bytes,
        s->pm_stack_remaining,
        rssi_mean, rssi_std, s->rssi_min, s->rssi_max,
        s->events_logged, s->events_dropped
    );
}

/* ---- Summary print ---- */

void metrics_print_summary(void) {
    metrics_snapshot_t s;
    metrics_get_snapshot(&s);

    Serial.println("\n=== BLUE METRICS ===");
    Serial.printf("WiFi: %lu connects, %.1f%% uptime, avg connect %.0fms, worst %.0fms\n",
                  s.wifi_connect_count, s.wifi_uptime_pct,
                  s.wifi_connect_time_ms_mean, s.wifi_connect_time_ms_p99);
    Serial.printf("Scan: %lu scans, avg %.0fms, worst %.0fms, avg %.0f APs\n",
                  s.scan_count, s.scan_time_ms_mean, s.scan_time_ms_p99,
                  s.scan_ap_count_mean);
    Serial.printf("ESPNOW: %lu tx, %lu fail, %lu rx\n",
                  s.espnow_tx_count, s.espnow_tx_fail_count, s.espnow_rx_count);
    Serial.printf("Power: %lu sleeps (%lu ms), %lu repeater activations (%lu ms)\n",
                  s.sleep_count, s.sleep_total_ms,
                  s.repeater_active_count, s.repeater_total_ms);
    Serial.printf("Latency: avg %.0fms, worst %.0fms, deadline %lums — %lu met, %lu missed\n",
                  s.activation_latency_ms_mean, s.activation_latency_ms_p99,
                  s.activation_deadline_ms, s.deadline_met_count, s.deadline_missed_count);
    Serial.printf("RSSI: mean %.1f dBm, stddev %.1f, range [%d, %d]\n",
                  s.rssi_mean, s.rssi_stddev, s.rssi_min, s.rssi_max);
    Serial.printf("Heap: %lu free (min %lu)\n",
                  s.free_heap_bytes, s.min_free_heap_bytes);
    Serial.printf("Events: %lu logged, %lu dropped\n",
                  s.events_logged, s.events_dropped);

    for (uint8_t i = 0; i < METRICS_MAX_TRACKED_OPS; i++) {
        if (ops[i].count > 0) {
            float avg_us = (float)ops[i].total_time_us / (float)ops[i].count;
            const char *names[] = {"WIFI_CONN", "SCAN", "ESPNOW_TX", "ESPNOW_RX",
                                   "SLEEP", "HTTP_POST", "RF_BUILD", "STATE_MACH"};
            Serial.printf("  OP[%s]: %lu calls, avg %.0fus, WCET %.0fus, %lu fails\n",
                          names[i], ops[i].count, avg_us,
                          (float)ops[i].wcet_us, ops[i].fail_count);
        }
    }
    Serial.println("====================\n");
}

/* ---- Track TX/RX counts (called from espnow_comm callbacks) ---- */

void metrics_track_tx(void) { espnow_tx_count++; }
void metrics_track_tx_fail(void) { espnow_tx_fail_count++; }
void metrics_track_rx(void) { espnow_rx_count++; }
