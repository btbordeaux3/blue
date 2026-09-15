#ifndef METRICS_H
#define METRICS_H

#include <Arduino.h>
#include "common.h"

/*
 * BLUE Real-Time System Metrics
 *
 * Tracks deterministic timing guarantees, worst-case execution times (WCET),
 * WiFi availability statistics, and FreeRTOS task health. All statistics are
 * computed on-device using Welford's online algorithm for numerically stable
 * mean/variance calculation. Events are stored in a fixed-size ring buffer
 * with monotonic sequence numbers for overflow detection.
 *
 * This module is designed for interview-level embedded systems work:
 *   - Bounded memory usage (no dynamic allocation)
 *   - Lock-free ring buffer for ISR-safe event logging
 *   - Online statistics that don't require storing all samples
 *   - WCET tracking using hardware timer reads
 *   - Stack high-water mark monitoring for RTOS task health
 */

#define METRICS_EVENT_LOG_SIZE 64
#define METRICS_MAX_TRACKED_OPS 8

/* Event types for the ring buffer log */
enum metrics_event_type_t : uint8_t {
    EVT_WIFI_CONNECT       = 0x01,
    EVT_WIFI_DISCONNECT    = 0x02,
    EVT_SCAN_START         = 0x03,
    EVT_SCAN_COMPLETE      = 0x04,
    EVT_ESPNOW_TX          = 0x05,
    EVT_ESPNOW_RX          = 0x06,
    EVT_SLEEP_ENTER        = 0x07,
    EVT_SLEEP_WAKE         = 0x08,
    EVT_REPEATER_ACTIVATE  = 0x09,
    EVT_REPEATER_DEACTIVATE = 0x0A,
    EVT_STATE_TRANSITION   = 0x0B,
    EVT_WATCHDOG_RESET     = 0x0C,
    EVT_HEAP_LOW           = 0x0D,
    EVT_STACK_WARNING      = 0x0E,
    EVT_ACTIVATION_LATENCY = 0x0F,
    EVT_DEADLINE_MET       = 0x10,
    EVT_DEADLINE_MISSED    = 0x11,
};

/* A single timestamped event in the ring buffer */
struct metrics_event_t {
    uint32_t timestamp_ms;
    uint32_t value;
    uint8_t  type;
    uint8_t  extra;
};

/*
 * Welford's online algorithm state for a single metric.
 *
 * This computes running mean and variance without storing all samples.
 * Interview talking point: Welford's algorithm is numerically stable
 * (no catastrophic cancellation) and uses O(1) memory vs O(n) for
 * storing all samples. The recurrence relations are:
 *   M_n = M_{n-1} + (x_n - M_{n-1}) / n
 *   S_n = S_{n-1} + (x_n - M_{n-1}) * (x_n - M_n)
 *   variance = S_n / (n - 1)   [Bessel's correction]
 */
struct welford_state_t {
    uint32_t count;
    float    mean;
    float    m2;       /* sum of squared differences from mean */
    int32_t  min;
    int32_t  max;
};

/*
 * Aggregated metrics snapshot, computed on-demand and pushed to the API.
 * All fields are populated by metrics_get_snapshot().
 */
struct metrics_snapshot_t {
    /* WiFi timing */
    uint32_t wifi_connect_count;
    uint32_t wifi_connect_fail_count;
    float    wifi_connect_time_ms_mean;
    float    wifi_connect_time_ms_p99;     /* worst 1% = max observed */

    /* Scan timing */
    uint32_t scan_count;
    float    scan_time_ms_mean;
    float    scan_time_ms_p99;
    int32_t  scan_ap_count_mean;

    /* ESPNOW */
    uint32_t espnow_tx_count;
    uint32_t espnow_tx_fail_count;
    uint32_t espnow_rx_count;
    float    espnow_tx_time_ms_mean;

    /* WiFi availability (uptime percentage) */
    uint32_t wifi_connected_ms;
    uint32_t wifi_disconnected_ms;
    float    wifi_uptime_pct;

    /* Power management */
    uint32_t sleep_count;
    uint32_t sleep_total_ms;
    uint32_t repeater_active_count;
    uint32_t repeater_total_ms;

    /* Repeater activation latency */
    float    activation_latency_ms_mean;
    float    activation_latency_ms_p99;
    uint32_t activation_deadline_ms;
    uint32_t deadline_met_count;
    uint32_t deadline_missed_count;

    /* FreeRTOS health */
    uint32_t free_heap_bytes;
    uint32_t min_free_heap_bytes;
    uint16_t pm_stack_remaining;
    uint16_t scan_stack_remaining;
    uint16_t report_stack_remaining;
    uint16_t repeater_stack_remaining;

    /* RSSI statistics */
    float    rssi_mean;
    float    rssi_stddev;
    int32_t  rssi_min;
    int32_t  rssi_max;

    /* Event log metadata */
    uint32_t events_logged;
    uint32_t events_dropped;
};

/*
 * Operation timing tracker.
 *
 * Usage:
 *   uint32_t id = metrics_begin_op(OP_WIFI_CONNECT);
 *   ... do work ...
 *   metrics_end_op(id, success);
 *
 * This tracks count, total time, WCET, and failure count per operation.
 * The WCET is the single longest execution time ever observed -- this is
 * the critical metric for real-time systems because it defines the worst
 * case scheduling bound.
 */
struct op_tracker_t {
    uint32_t count;
    uint32_t fail_count;
    uint32_t total_time_us;
    uint32_t wcet_us;           /* worst-case execution time */
    uint32_t last_start_us;
    bool     active;
};

enum metrics_op_t : uint8_t {
    OP_WIFI_CONNECT    = 0,
    OP_SCAN            = 1,
    OP_ESPNOW_SEND     = 2,
    OP_ESPNOW_RECV     = 3,
    OP_SLEEP           = 4,
    OP_HTTP_POST       = 5,
    OP_RF_BUILD_FP     = 6,
    OP_STATE_MACHINE   = 7,
};

/* ---- API ---- */

void     metrics_init(void);

/* Event logging (ring buffer) */
void     metrics_log_event(uint8_t type, uint32_t value, uint8_t extra);
uint32_t metrics_get_events(metrics_event_t *out, uint32_t max_count);
uint32_t metrics_get_event_count(void);
uint32_t metrics_get_events_dropped(void);

/* Operation timing (WCET tracker) */
uint32_t metrics_begin_op(uint8_t op);
void     metrics_end_op(uint32_t handle, bool success);

/* On-device statistics (Welford's) */
void     metrics_record_rssi(int32_t rssi);
void     metrics_record_scan_ap_count(uint8_t count);
void     metrics_record_wifi_connect_time(uint32_t ms);
void     metrics_record_scan_time(uint32_t ms);
void     metrics_record_espnow_tx_time(uint32_t us);

/* State tracking */
void     metrics_on_wifi_connect(void);
void     metrics_on_wifi_disconnect(void);
void     metrics_on_sleep_enter(void);
void     metrics_on_sleep_wake(void);
void     metrics_on_repeater_activate(void);
void     metrics_on_repeater_deactivate(void);

/* Activation latency (weak signal → repeater started) */
void     metrics_set_weak_signal_time(uint32_t timestamp_ms);
void     metrics_record_activation_latency(uint32_t started_ms);

/* Which task to track stack high-water marks for. Slots map onto the
 * dashboard's Task Stacks card so each node reports the tasks it actually
 * runs (base: scan; mobile: pm/scan/report/repeater). */
enum metrics_task_slot_t : uint8_t {
    METRICS_TASK_PM       = 0,
    METRICS_TASK_SCAN     = 1,
    METRICS_TASK_REPORT   = 2,
    METRICS_TASK_REPEATER = 3,
    METRICS_TASK_SLOTS    = 4
};

/* Health monitoring (call periodically from a task) */
void     metrics_check_task_health(void);
void     metrics_register_task(uint8_t slot, TaskHandle_t handle);
void     metrics_set_pm_handle(TaskHandle_t handle);

/* Snapshot for API push */
void     metrics_get_snapshot(metrics_snapshot_t *out);
void     metrics_print_summary(void);

/* JSON serialization for API push */
int      metrics_snapshot_to_json(const metrics_snapshot_t *snap, char *buf, size_t buf_len);

/* TX/RX tracking (called from espnow_comm callbacks) */
void     metrics_track_tx(void);
void     metrics_track_tx_fail(void);
void     metrics_track_rx(void);

#endif
