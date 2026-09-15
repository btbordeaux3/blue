/*
 * main.cpp - BLUE Base Station (ESP32-S3)
 *
 * The base station is the stationary coordinator in the BLUE system.
 * It performs these roles simultaneously:
 *
 *   1. WiFi Scanner: periodically scans for APs and builds fingerprints
 *   2. ESPNOW Gateway: receives reports from mobile, sends commands
 *   3. Decision Engine: activates/deactivates mobile's repeater mode
 *   4. ML Predictor: predicts weak zones before they happen
 *   5. Cloud Push: forwards fingerprints and metrics to Cloudflare Workers API
 *   6. Storage: persists RF map data to SPIFFS for ML training
 *   7. Metrics: collects WCET, task health, and system statistics
 *
 * Task architecture (FreeRTOS):
 *   - Core 0: command_task, push_task, storage_task (I/O-bound)
 *   - Core 1: scan_task (WiFi radio, blocking scans)
 *   - Timer: heartbeat (every 10s, from ISR-safe timer callback)
 *   - Loop: metrics health check + periodic summary print
 *
 * Interview talking points:
 *   - Dual-core ESP32-S3: WiFi on core 1, logic on core 0
 *   - NTP sync via configTime() for timestamped RF map entries
 *   - Software timer for heartbeats (not vTaskDelay) for drift-free timing
 *   - SPIFFS for persistent storage (wear-leveled flash)
 *   - Union-based message queue for mixed fingerprint/status/metrics push
 */

#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <time.h>
#include "common.h"
#include "rf_fingerprint.h"
#include "ml_model.h"
#include "espnow_comm.h"
#include "scan_task.h"
#include "command_task.h"
#include "storage_task.h"
#include "push_task.h"
#include "metrics.h"

static uint8_t base_mac[6];
static uint8_t mobile_mac[6] = {0x8C, 0x94, 0xDF, 0x71, 0x4F, 0xB0};

#ifndef API_URL
#define API_URL "https://rf-map-api.example.workers.dev"
#endif

static void get_mac(uint8_t *mac) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    WiFi.macAddress(mac);
}

static TimerHandle_t heartbeat_timer;

static void heartbeat_callback(TimerHandle_t timer) {
    heartbeat_msg_t hb;
    hb.msg_type = MSG_HEARTBEAT;
    hb.timestamp = millis();
    hb.sequence_num = 0;
    hb.connection_quality = (uint8_t)espnow_get_connection_status();
    hb.active_clients = command_task_get_active_clients();
    espnow_send_heartbeat(&hb);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n========================================");
    Serial.println("  BLUE BASE STATION");
    Serial.println("  Adaptive WiFi Repeater - Base Node");
    Serial.println("========================================\n");

    get_mac(base_mac);
    Serial.printf("[BASE] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  base_mac[0], base_mac[1], base_mac[2],
                  base_mac[3], base_mac[4], base_mac[5]);

    if (!SPIFFS.begin(true)) {
        Serial.println("[STORAGE] SPIFFS mount failed");
    } else {
        Serial.println("[STORAGE] SPIFFS mounted");
    }

    if (!espnow_comm_init(DEVICE_BASE, mobile_mac)) {
        Serial.println("[ESPNOW] init failed - halting");
        while (1) { delay(1000); }
    }

    rf_map_init();
    storage_task_load_map();
    ml_task_setup();

    push_task_init(API_URL);

    scan_task_init();
    command_task_init();
    storage_task_init();
    metrics_init();

    heartbeat_timer = xTimerCreate("hb", pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS),
                                    pdTRUE, NULL, heartbeat_callback);

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[BASE] Connecting to WiFi");
    int wifi_attempts = 0;
    while (WiFi.status() != WL_CONNECTED && wifi_attempts < 30) {
        delay(500);
        Serial.print(".");
        wifi_attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[BASE] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 5000)) {
            Serial.printf("[BASE] NTP synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                          timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        } else {
            Serial.println("[BASE] NTP sync failed - timestamps will be wrong");
        }
        uint8_t ch = espnow_sync_channel();
        Serial.printf("[BASE] ESPNOW peer synced to channel %d\n", ch);

        /* Record the base's own uplink connect so the dashboard metrics
         * (up-time %, connect count, avg connect ms) reflect reality. */
        metrics_on_wifi_connect();
        metrics_record_wifi_connect_time((uint32_t)wifi_attempts * 500);
    } else {
        Serial.println("\n[BASE] WiFi connection failed - push disabled");
    }

    scan_task_start();
    command_task_start();
    storage_task_start();
    push_task_start();

    xTimerStart(heartbeat_timer, 0);

    Serial.println("[BASE] All tasks started, entering main loop\n");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
    espnow_print_peers();
    rf_map_print_stats();
    metrics_check_task_health();
    static uint32_t last_metrics_push = 0;
    if (millis() - last_metrics_push > 60000) {
        metrics_snapshot_t snap;
        metrics_get_snapshot(&snap);
        push_task_send_metrics(&snap);
        last_metrics_push = millis();
    }
}
