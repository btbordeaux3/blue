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
#include "esp_wifi.h"
#include "esp_event.h"

static TaskHandle_t scan_handle = NULL;
static rf_fingerprint_t latest_fp;
static bool has_fp = false;
static SemaphoreHandle_t fp_mutex = NULL;

static void mobile_scan_task(void *param) {
    Serial.println("[SCAN-M] Task started");
    bool first_scan = true;

    while (1) {
        if (first_scan) {
            while (WiFi.status() != WL_CONNECTED) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
            Serial.printf("[SCAN-M] Initial WiFi connected, IP: %s, Channel: %d\n", 
                          WiFi.localIP().toString().c_str(), WiFi.channel());
            first_scan = false;
        }

        /* Ensure WiFi radio is ready after sleep */
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[SCAN-M] WiFi not connected, reconnecting...");
            char nssid[33];
            char npsk[65];
            repeater_task_get_credentials(nssid, sizeof(nssid), npsk, sizeof(npsk));
            WiFi.disconnect();
            WiFi.mode(WIFI_STA);
            WiFi.begin(nssid, npsk);
            uint32_t wait = 0;
            while (WiFi.status() != WL_CONNECTED && wait < 30) {
                vTaskDelay(pdMS_TO_TICKS(500));
                wait++;
            }
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("[SCAN-M] Reconnected, Channel: %d\n", WiFi.channel());
                espnow_sync_channel();
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        /* Use ESP-IDF scan API directly */
        uint32_t scan_start = millis();
        metrics_log_event(EVT_SCAN_START, 0, 0);
        
        wifi_scan_config_t scan_config = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,  // all channels
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time = { .active = { .min = 50, .max = 120 } },
        };
        
        esp_err_t err = esp_wifi_scan_start(&scan_config, true);  // blocking
        uint32_t scan_elapsed = millis() - scan_start;
        metrics_record_scan_time(scan_elapsed);
        metrics_log_event(EVT_SCAN_COMPLETE, scan_elapsed, 0);

        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        
        Serial.printf("[SCAN-M] ESP-IDF scan done: %d APs found (took %lu ms, err=%d)\n", 
                      ap_count, scan_elapsed, err);

        if (err != ESP_OK || ap_count == 0) {
            Serial.println("[SCAN-M] Scan empty/failed, retrying...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(ap_count * sizeof(wifi_ap_record_t));
        if (ap_records) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_records);
            
            rf_scan_result_t results[32];
            uint8_t count = 0;
            for (int i = 0; i < ap_count && count < 32; i++) {
                results[count].channel = ap_records[i].primary;
                results[count].rssi = ap_records[i].rssi;
                results[count].authmode = ap_records[i].authmode;
                strncpy(results[count].ssid, (char*)ap_records[i].ssid, 32);
                results[count].ssid[32] = '\0';
                memcpy(results[count].bssid, ap_records[i].bssid, 6);
                count++;
            }
            free(ap_records);

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
        } else {
            Serial.println("[SCAN-M] Failed to alloc scan results");
        }

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
    metrics_register_task(METRICS_TASK_SCAN, scan_handle);
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