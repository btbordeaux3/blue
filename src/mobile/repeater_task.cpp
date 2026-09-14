/*
 * repeater_task.cpp - Adaptive WiFi repeater bridge
 *
 * When activated, the mobile unit creates a softAP (access point) that
 * mirrors the main router's SSID, then connects to the router as a
 * station. This creates a dual-role bridge:
 *
 *   [Client] → [Mobile AP] → [Mobile STA] → [Router]
 *
 * Internet is provided by NAPT (Network Address Port Translation /
 * masquerade) on the AP netif: softAP clients receive a 192.168.4.x lease
 * from the AP's DHCP server, and every packet is NAT'ed out the STA link by
 * the lwIP netif. Without this the clients "connect" but have no route to
 * the internet - which is exactly the "extender has no internet" symptom.
 *
 * The AP is locked to the router's WiFi channel (same-channel AP+STA keeps
 * the ESP32 radio on one channel, avoiding the channel-hopping that would
 * drop every client).
 *
 * NAPT is enabled on the STA "got IP" event and torn down on disconnect,
 * exactly like the framework's canonical WiFiExtender example, so a router
 * blip recovers automatically instead of leaving the bridge half-wired.
 *
 * Key design decisions:
 *   - WiFi.AP.* (Network API, core 3.x) instead of legacy WiFi.softAP():
 *     only the Network API can enable NAPT (esp_netif_napt_enable).
 *   - 500ms delay isn't needed: create() waits on the started bit.
 *   - Max 4 clients (AP queue limit for memory-constrained ESP32)
 *   - Active-low LED: LOW = on, HIGH = off (ESP32 GPIO default is HIGH at boot)
 */

#include "repeater_task.h"
#include "common.h"
#include <WiFi.h>
#include <Network.h>
#include <string.h>

static TaskHandle_t repeater_handle = NULL;
static bool bridge_active = false;
static uint8_t client_count = 0;
static uint32_t bridge_started_at = 0;

/* Router credentials: default from build, overridable at runtime from the
 * host's provisioning ("zero-config mobile node", see usb_link). */
static char router_ssid[33] = WIFI_SSID;
static char router_psk[64]  = WIFI_PASS;

static void on_wifi_event(arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            if (bridge_active) {
                if (WiFi.AP.enableNAPT(true)) {
                    Serial.println("[REPEATER] NAPT enabled");
                } else {
                    Serial.println("[REPEATER] NAPT enable FAILED");
                }
            }
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            if (bridge_active) {
                WiFi.AP.enableNAPT(false);
                Serial.println("[REPEATER] NAPT disabled (STA link lost)");
            }
            break;
        default:
            break;
    }
}

void repeater_task_set_credentials(const char *ssid, const char *psk)
{
    if (!ssid || !psk) return;
    strncpy(router_ssid, ssid, sizeof(router_ssid) - 1);
    router_ssid[sizeof(router_ssid) - 1] = '\0';
    strncpy(router_psk, psk, sizeof(router_psk) - 1);
    router_psk[sizeof(router_psk) - 1] = '\0';
    Serial.printf("[REPEATER] router credentials set to '%s'\n", router_ssid);
}

void repeater_task_get_credentials(char *ssid, size_t ssid_sz, char *psk, size_t psk_sz)
{
    if (ssid && ssid_sz)
        snprintf(ssid, ssid_sz, "%s", router_ssid);
    if (psk && psk_sz)
        snprintf(psk, psk_sz, "%s", router_psk);
}

static bool join_router(int max_attempts)
{
    if (WiFi.status() == WL_CONNECTED) return true;

    WiFi.begin(router_ssid, router_psk);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < max_attempts) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
    }
    return WiFi.status() == WL_CONNECTED;
}

static void repeater_task(void *param) {
    Serial.println("[REPEATER] Task started");

    while (1) {
        if (bridge_active) {
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[REPEATER] Lost connection to router, attempting reconnect...");
                WiFi.AP.enableNAPT(false);
                WiFi.disconnect();
                WiFi.begin(router_ssid, router_psk);
                int attempts = 0;
                while (WiFi.status() != WL_CONNECTED && attempts < 20) {
                    vTaskDelay(pdMS_TO_TICKS(500));
                    attempts++;
                }
                if (WiFi.status() == WL_CONNECTED) {
                    WiFi.AP.enableNAPT(true);
                } else {
                    Serial.println("[REPEATER] Reconnect failed");
                }
            }

            client_count = WiFi.AP.stationCount();
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void repeater_task_init(void) {
    Network.onEvent(on_wifi_event);
    repeater_task_set_credentials(WIFI_SSID, WIFI_PASS);
    Serial.println("[REPEATER] Initialized");
}

void repeater_task_start(void) {
    xTaskCreatePinnedToCore(repeater_task, "repeater", 8192, NULL, 4, &repeater_handle, 1);
}

void repeater_task_activate(void) {
    if (bridge_active) return;

    Serial.println("[REPEATER] Activating bridge...");

    /* Station (router) link first so we can lock the AP to its channel. */
    if (!join_router(30)) {
        Serial.println("[REPEATER] Cannot start bridge - router not reachable");
        return;
    }

    uint8_t channel = WiFi.channel();
    if (channel == 0) channel = 7;

    /* AP subnet (192.168.4.0/24): DHCP lease range + DNS handed to clients.
     * NAPT then masquerades them out the STA interface. */
    IPAddress ap_ip(192, 168, 4, 1);
    IPAddress ap_mask(255, 255, 255, 0);
    IPAddress ap_lease_start(192, 168, 4, 10);
    IPAddress ap_dns(8, 8, 8, 8);

    if (!WiFi.AP.begin()) {
        Serial.println("[REPEATER] AP begin failed");
        WiFi.mode(WIFI_STA);
        return;
    }
    WiFi.AP.config(ap_ip, ap_ip, ap_mask, ap_lease_start, ap_dns);
    if (!WiFi.AP.create(REPEATER_SSID, REPEATER_PASS, channel, 0, 4)) {
        Serial.println("[REPEATER] AP create failed");
        WiFi.mode(WIFI_STA);
        return;
    }
    Serial.printf("[REPEATER] AP started: %s on channel %d\n", REPEATER_SSID, channel);

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[REPEATER] Connected to %s, IP: %s\n",
                      router_ssid, WiFi.localIP().toString().c_str());
        bridge_active = true;
        bridge_started_at = millis();
    digitalWrite(REPEATER_LED_PIN, LOW);
        if (!WiFi.AP.enableNAPT(true)) {
            Serial.println("[REPEATER] NAPT enable FAILED - clients will have no internet");
        }
        Serial.println("[REPEATER] Bridge ACTIVE (NAPT)");
    } else {
        Serial.println("[REPEATER] Failed to connect to router");
        WiFi.AP.end();
        WiFi.mode(WIFI_STA);
    }
}

void repeater_task_deactivate(void) {
    if (!bridge_active) return;

    Serial.println("[REPEATER] Deactivating bridge...");

    WiFi.AP.enableNAPT(false);
    WiFi.AP.end();
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);

    bridge_active = false;
    client_count = 0;
    bridge_started_at = 0;
    digitalWrite(REPEATER_LED_PIN, HIGH);

    Serial.println("[REPEATER] Bridge DEACTIVATED");
}

bool repeater_task_is_active(void) {
    return bridge_active;
}

uint8_t repeater_task_get_client_count(void) {
    return client_count;
}