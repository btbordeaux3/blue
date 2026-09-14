#include "push_task.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include "metrics.h"
#include "common.h"

static TaskHandle_t push_handle = NULL;
static QueueHandle_t push_queue = NULL;
static char api_base_url[128] = {0};

/* Cloud pushes are BATCHED: the site polls every 10 s but the devices emit
 * fingerprints every ~10 s, which would exceed Cloudflare KV's free-tier
 * write budget (1,000 writes/day). Scan results accumulate here and are
 * flushed once per CLOUD_PUSH_INTERVAL_MS as a single compact POST whose
 * whole body stays under the ~1 KB ESP32 HTTPS send limit. */
#define CLOUD_PUSH_INTERVAL_MS 120000
#define METRICS_FLUSH_MS       600000

/* Kept as a rolling latest-snapshot per type. */
typedef struct {
    int64_t       ts;
    uint8_t       count;
    uint32_t      bssid_hashes[TOP_N_RSSI];
    rssi_t        rssi_values[TOP_N_RSSI];
} fp_snapshot_t;

typedef struct {
    uint8_t type;           /* 0 = fingerprint, 1 = status, 2 = metrics, 3 = decision */
    uint8_t _pad[3];
    union {
        struct {
            rf_fingerprint_t fp;
            uint32_t total_entries;
            uint32_t weak_count;
        };
        metrics_snapshot_t metrics;
        push_decision_t    decision;
    };
} push_msg_t;

static fp_snapshot_t   last_fp;
static metrics_snapshot_t last_metrics;
static bool            last_metrics_valid = false;
static push_decision_t last_decision;
static bool            last_decision_valid = false;
static uint32_t        last_total = 0;
static uint32_t        last_weak  = 0;

static bool post_json(const char *path, const char *json) {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    String url = String(api_base_url) + path;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    int code = http.POST(json);
    http.end();

    if (code == 200 || code == 201) {
        Serial.printf("[PUSH] OK %s (%d)\n", path, code);
        return true;
    } else {
        Serial.printf("[PUSH] FAIL %s (%d)\n", path, code);
        return false;
    }
}

static void flush_fingerprint(void) {
    if (last_fp.count == 0) return;

    struct tm timeinfo;
    time_t now_ts;
    time(&now_ts);
    localtime_r(&now_ts, &timeinfo);

    /* Compact keys to fit all TOP_N_RSSI entries in one <1KB body. */
    char json[1024];
    int off = snprintf(json, sizeof(json), "[");
    for (uint8_t i = 0; i < last_fp.count && i < TOP_N_RSSI; i++) {
        int entry_len = snprintf(NULL, 0,
            "{\"h\":%lu,\"hr\":%d,\"d\":%d,\"r\":%d}",
            last_fp.bssid_hashes[i], timeinfo.tm_hour, timeinfo.tm_wday,
            last_fp.rssi_values[i]) + 1;
        if (off + entry_len + 2 > (int)sizeof(json)) break;
        if (i > 0) off += snprintf(json + off, sizeof(json) - off, ",");
        off += snprintf(json + off, sizeof(json) - off,
            "{\"h\":%lu,\"hr\":%d,\"d\":%d,\"r\":%d}",
            last_fp.bssid_hashes[i], timeinfo.tm_hour, timeinfo.tm_wday,
            last_fp.rssi_values[i]);
    }
    off += snprintf(json + off, sizeof(json) - off, "]");

    post_json("/api/report", json);
}

static void flush_metrics(void) {
    if (!last_metrics_valid) return;
    char json[1024];
    metrics_snapshot_to_json(&last_metrics, json, sizeof(json));
    post_json("/api/metrics", json);
}

static void flush_decision(void) {
    if (!last_decision_valid) return;
    const char *src = "none";
    if (last_decision.source == SRC_BASE_ML)          src = "base_ml";
    else if (last_decision.source == SRC_MOBILE_RSSI) src = "mobile_low";
    else if (last_decision.source == SRC_BASE_MANUAL) src = "base_manual";
    char json[320];
    snprintf(json, sizeof(json),
        "{\"active\":%u,\"source\":%u,\"source_name\":\"%s\","
        "\"confidence\":%.2f,\"mobile_state\":%u,"
        "\"base_activation_count\":%lu,"
        "\"model\":{\"trained\":%u,\"samples\":%u,\"pos\":%u,\"neg\":%u}}",
        last_decision.active, last_decision.source, src,
        last_decision.confidence, last_decision.mobile_state,
        (unsigned long)last_decision.base_activation_count,
        last_decision.model_trained, last_decision.model_samples,
        last_decision.model_pos, last_decision.model_neg);
    post_json("/api/decision", json);
    last_decision_valid = false;   /* send once per change */
}

static void push_task(void *param) {
    Serial.println("[PUSH] Task started");

    while (1) {
        push_msg_t msg;
        if (xQueueReceive(push_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (msg.type) {
            case 0: /* fingerprint: keep the latest snapshot only */
                last_fp.count = (msg.fp.count < TOP_N_RSSI) ? msg.fp.count : TOP_N_RSSI;
                for (uint8_t i = 0; i < last_fp.count; i++) {
                    last_fp.bssid_hashes[i] = msg.fp.bssid_hashes[i];
                    last_fp.rssi_values[i]  = msg.fp.rssi_values[i];
                }
                last_fp.ts = millis();
                break;
            case 1: /* status: remember counts; no separate cloud write */
                last_total = msg.total_entries;
                last_weak  = msg.weak_count;
                break;
            case 2: /* metrics */
                memcpy(&last_metrics, &msg.metrics, sizeof(last_metrics));
                last_metrics_valid = true;
                break;
            case 3: /* decision */
                last_decision = msg.decision;
                last_decision_valid = true;
                break;
            }
        }

        /* Flush everything on the batch interval; decisions also flush
         * promptly (~1s) so the dashboard state is responsive. Metrics are
         * gated to METRICS_FLUSH_MS to stay inside the KV free-tier budget. */
        static uint32_t last_flush = 0;
        static uint32_t last_metrics_flush = 0;
        bool interval = (uint32_t)(millis() - last_flush) >= CLOUD_PUSH_INTERVAL_MS;
        bool metrics_due = (uint32_t)(millis() - last_metrics_flush) >= METRICS_FLUSH_MS;
        if (interval) {
            flush_fingerprint();
            last_flush = millis();
        }
        if (metrics_due) {
            flush_metrics();
            last_metrics_flush = millis();
        }
        /* Keep the dashboard's repeater card live even between batches. */
        if (last_decision_valid) {
            flush_decision();
        }
    }
}

void push_task_init(const char *api_url) {
    strncpy(api_base_url, api_url, sizeof(api_base_url) - 1);
    push_queue = xQueueCreate(6, sizeof(push_msg_t));
    Serial.printf("[PUSH] API: %s\n", api_base_url);
}

void push_task_start(void) {
    xTaskCreatePinnedToCore(push_task, "push", 8192, NULL, 1, &push_handle, 0);
}

bool push_task_send_fingerprint(const rf_fingerprint_t *fp) {
    push_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = 0;
    msg.fp = *fp;
    return xQueueSend(push_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool push_task_send_status(uint32_t total_entries, uint32_t weak_count) {
    push_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = 1;
    msg.total_entries = total_entries;
    msg.weak_count = weak_count;
    return xQueueSend(push_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool push_task_send_metrics(const metrics_snapshot_t *snap) {
    push_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = 2;
    memcpy(&msg.metrics, snap, sizeof(metrics_snapshot_t));
    return xQueueSend(push_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool push_task_send_decision(const push_decision_t *d) {
    push_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = 3;
    msg.decision = *d;
    return xQueueSend(push_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE;
}