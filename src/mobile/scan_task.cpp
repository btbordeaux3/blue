/*
 * mobile_scan_task.cpp - Mobile unit WiFi scanner
 *
 * Similar to base scan task but runs on battery-powered mobile unit.
 * Key differences:
 *   - Reconnects to WiFi after scan (scan disconnects temporarily)
 *   - Runs on core 1 alongside WiFi radio
 *   - Stopped/resumed by power manager during sleep cycles
 *   - Thread-safe fingerprint access via mutex
 *
 * Interview talking points:
 *   - WiFi.scanNetworks() temporarily disconnects from AP
 *   - Must call WiFi.begin() after scan to restore connection
 *   - vTaskSuspend/Resume for clean task lifecycle management
 *   - Semaphore-protected shared state (latest_fp) between scan and report tasks
 */

#include "mobile_scan_task.h"
#include <WiFi.h>
#include "espnow_comm.h"
#include "repeater_task.h"
#include "common.h"
#include "metrics.h"

static TaskHandle_t scan_handle = NULL;
static rf_fingerprint_t latest_fp;
static bool has_fp = false;
static SemaphoreHandle_t fp_mutex = NULL;

static void mobile_scan_task(void *param) {
    Serial.println("[SCAN-M] Task started");

    while (1) {
        uint32_t scan_start = millis();
        metrics_log_event(EVT_SCAN_START, 0, 0);
        int n = WiFi.scanNetworks(false, true);
        uint32_t scan_elapsed = millis() - scan_start;
        metrics_record_scan_time(scan_elapsed);
        metrics_log_event(EVT_SCAN_COMPLETE, scan_elapsed, (uint8_t)n);

        if (WiFi.status() != WL_CONNECTED) {
            char nssid[33];
            char npsk[65];
            repeater_task_get_credentials(nssid, sizeof(nssid), npsk, sizeof(npsk));
            WiFi.begin(nssid, npsk);
            uint32_t wait = 0;
            while (WiFi.status() != WL_CONNECTED && wait < 20) {
                vTaskDelay(pdMS_TO_TICKS(500));
                wait++;
            }
            if (WiFi.status() == WL_CONNECTED) {
                espnow_sync_channel();
            }
        }

        if (n == WIFI_SCAN_FAILED || n == 0) {
            vTaskDelay(pdMS_TO_TICKS(2000));
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

        rf_fingerprint_t fp;
        rf_build_fingerprint(results, count, &fp);

        if (xSemaphoreTake(fp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            memcpy(&latest_fp, &fp, sizeof(rf_fingerprint_t));
            has_fp = true;
            xSemaphoreGive(fp_mutex);
        }

        Serial.printf("[SCAN-M] %u APs, avg RSSI: %d dBm, weak: %s\n",
                      fp.count, rf_average_top_n(&fp, 3),
                      rf_is_weak_signal(&fp) ? "YES" : "no");

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void mobile_scan_task_init(void) {
    fp_mutex = xSemaphoreCreateMutex();
}

void mobile_scan_task_start(void) {
    if (scan_handle == NULL) {
        xTaskCreatePinnedToCore(mobile_scan_task, "scan_m", 8192, NULL, 3, &scan_handle, 1);
    } else {
        vTaskResume(scan_handle);
    }
}

void mobile_scan_task_stop(void) {
    if (scan_handle != NULL) {
        vTaskSuspend(scan_handle);
    }
}

bool mobile_scan_get_fingerprint(rf_fingerprint_t *fp) {
    if (xSemaphoreTake(fp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (has_fp) {
            memcpy(fp, &latest_fp, sizeof(rf_fingerprint_t));
            xSemaphoreGive(fp_mutex);
            return true;
        }
        xSemaphoreGive(fp_mutex);
    }
    return false;
}
