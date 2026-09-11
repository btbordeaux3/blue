/*
 * main.cpp - BLUE Mobile Unit (ESP32 Classic)
 *
 * The mobile unit is the battery-powered sensor node in the BLUE system.
 * It performs these roles:
 *
 *   1. WiFi Scanner: periodically scans and builds RF fingerprints
 *   2. ESPNOW Reporter: sends fingerprints to base station
 *   3. Repeater Bridge: activates WiFi relay when in weak zone
 *   4. Power Manager: sleeps between scans to conserve battery
 *   5. Metrics: tracks timing, health, and power consumption
 *
 * Task architecture (FreeRTOS):
 *   - Core 0: power_manager, report_task (low-priority housekeeping)
 *   - Core 1: scan_task (WiFi radio, blocking scans)
 *   - Core 1: repeater_task (bridge management)
 *   - Loop: metrics health check + periodic summary
 *
 * Key differences from base:
 *   - ESP32 classic (not S3) - single-core with FreeRTOS on second core
 *   - Light sleep between scans for power savings
 *   - Ring buffer in report_task for offline data buffering
 *   - LED on GPIO 2 (active-low) indicates repeater status
 *
 * Interview talking points:
 *   - WiFi.scanNetworks() is async but blocks during actual scan
 *   - WiFi.begin() after scan reconnects to AP (scan disconnects temporarily)
 *   - Metrics via setter pattern (not #ifdef) to avoid shared-code linker issues
 *   - Power budget: ~80mA active WiFi, ~10mA light sleep
 */

#include <Arduino.h>
#include <WiFi.h>
#include "common.h"
#include "rf_fingerprint.h"
#include "espnow_comm.h"
#include "power_manager.h"
#include "mobile_scan_task.h"
#include "report_task.h"
#include "repeater_task.h"
#include "metrics.h"
#include "usb_link.h"
#include "oled_task.h"

static uint8_t mobile_mac[6];
static uint8_t base_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Host command channel: {"cmd":"ap","state":N} over the USB-C serial link. */
static void on_usb_ap_cmd(bool up) {
    if (up) {
        power_manager_request_activate();
    } else {
        power_manager_request_deactivate(DEACT_BASE_CMD);
    }
}

/* Host provisioning: {"cmd":"net","ssid":"...","psk":"..."}. Update the
 * router creds the repeater uses, and join the new network right away if we
 * are not currently acting as an extender. */
static void on_usb_net(const char *ssid, const char *psk) {
    repeater_task_set_credentials(ssid, psk);
    if (!repeater_task_is_active()) {
        WiFi.disconnect();
        WiFi.begin(ssid, psk);
        Serial.printf("[MOBILE] switched to router '%s'\n", ssid);
    }
}

static void get_mac(uint8_t *mac) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    WiFi.macAddress(mac);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    usb_link_set_ap_cmd_cb(on_usb_ap_cmd);
    usb_link_set_net_cb(on_usb_net);
    usb_link_init();
    Serial.println("\n========================================");
    Serial.println("  BLUE MOBILE UNIT");
    Serial.println("  Adaptive WiFi Repeater - Mobile Node");
    Serial.println("========================================\n");

    get_mac(mobile_mac);
    Serial.printf("[MOBILE] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mobile_mac[0], mobile_mac[1], mobile_mac[2],
                  mobile_mac[3], mobile_mac[4], mobile_mac[5]);

    if (!espnow_comm_init(DEVICE_MOBILE, base_mac)) {
        Serial.println("[ESPNOW] init failed - halting");
        while (1) { delay(1000); }
    }

    Serial.print("[MOBILE] Syncing WiFi channel");
    char nssid[USB_LINK_SSID_MAX + 1];
    char npsk[USB_LINK_PSK_MAX + 1];
    const char *boot_ssid = WIFI_SSID;
    const char *boot_psk  = WIFI_PASS;
    if (usb_link_get_net(nssid, sizeof(nssid), npsk, sizeof(npsk))) {
        boot_ssid = nssid;
        boot_psk  = npsk;
        Serial.printf("\n[MOBILE] using host-provisioned network '%s'\n", nssid);
    }
    WiFi.begin(boot_ssid, boot_psk);
    int wifi_attempts = 0;
    while (WiFi.status() != WL_CONNECTED && wifi_attempts < 20) {
        delay(500);
        Serial.print(".");
        wifi_attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[MOBILE] Synced to channel %d\n", WiFi.channel());
        uint8_t peer_ch = espnow_sync_channel();
        Serial.printf("[MOBILE] ESPNOW peer synced to channel %d\n", peer_ch);
    } else {
        Serial.println("\n[MOBILE] WiFi sync failed - using default channel");
    }

    power_manager_init();
    mobile_scan_task_init();
    report_task_init();
    repeater_task_init();
    oled_task_init();
    metrics_init();

    pinMode(REPEATER_LED_PIN, OUTPUT);
    digitalWrite(REPEATER_LED_PIN, HIGH);

    power_manager_start();
    mobile_scan_task_start();
    report_task_start();
    repeater_task_start();
    oled_task_start();

    Serial.println("[MOBILE] All tasks started\n");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(5000));
    usb_link_poll();
    power_manager_print_status();
    espnow_print_peers();
    metrics_check_task_health();
    static uint32_t last_metrics_print = 0;
    if (millis() - last_metrics_print > 60000) {
        metrics_print_summary();
        last_metrics_print = millis();
    }
}
