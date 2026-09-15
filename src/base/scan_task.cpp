/*
 * scan_task.cpp - Base station WiFi scanner
 *
 * Periodically scans for WiFi APs and builds RF fingerprints.
 * Runs on core 1 (with WiFi radio) to avoid blocking core 0 logic.
 *
 * Interview talking points:
 *   - WiFi.scanNetworks(false, true): synchronous scan, show hidden
 *   - After scan, WiFi.scanDelete() frees ESP-IDF scan buffer memory
 *   - Metrics instrumentation: scan timing, AP count, RSSI recording
 *   - No WiFi.begin() after scan (unlike mobile) because base stays connected
 */

#include "scan_task.h"
#include "espnow_comm.h"
#include "command_task.h"
#include "push_task.h"
#include "common.h"
#include "metrics.h"

static TaskHandle_t scan_task_handle = NULL;
static rf_fingerprint_t latest_fingerprint;
static bool has_fingerprint = false;

static void scan_task(void *param) {
    Serial.println("[SCAN] Task started");
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        uint32_t scan_start = millis();
        metrics_log_event(EVT_SCAN_START, 0, 0);
        int n = WiFi.scanNetworks(false, true);
        uint32_t scan_elapsed = millis() - scan_start;
        metrics_record_scan_time(scan_elapsed);
        metrics_log_event(EVT_SCAN_COMPLETE, scan_elapsed, (uint8_t)n);

        if (n == WIFI_SCAN_FAILED || n == 0) {
            vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
            continue;
        }

        rf_scan_result_t results[32];
        uint8_t count = 0;
        for (int i = 0; i < n && count < 32; i++) {
            results[count].channel = WiFi.channel(i);
            results[count].rssi = WiFi.RSSI(i);
            results[count].authmode = WiFi.encryptionType(i);
            strncpy(results[count].ssid, WiFi.SSID(i).c_str(), 32);
            results[count].ssid[32] = '\0';
            memcpy(results[count].bssid, WiFi.BSSID(i), 6);
            count++;
        }
        WiFi.scanDelete();

        rf_build_fingerprint(results, count, &latest_fingerprint);
        has_fingerprint = true;

        /* RSSI metric = link quality of the strongest APs (what matters for
         * connectivity), not the mean over every neighbor including the
         * barely-visible -98 dBm ones in the walls. */
        metrics_record_rssi(rf_average_valid_top_n(&latest_fingerprint, 3));
        metrics_record_scan_ap_count(count);

        Serial.printf("[SCAN] %u APs, avg RSSI: %d dBm\n",
                      latest_fingerprint.count,
                      rf_average_top_n(&latest_fingerprint, 3));

        push_task_send_fingerprint(&latest_fingerprint);

        command_task_notify_scan_done();

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}

void scan_task_init(void) {
}

void scan_task_start(void) {
    xTaskCreatePinnedToCore(scan_task, "scan", 8192, NULL, 3, &scan_task_handle, 1);
    metrics_register_task(METRICS_TASK_SCAN, scan_task_handle);
}

bool scan_task_get_fingerprint(rf_fingerprint_t *fp) {
    if (!has_fingerprint) return false;
    *fp = latest_fingerprint;
    return true;
}
