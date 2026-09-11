/*
 * usb_link.cpp - Implementation of the USB-C telemetry/command channel.
 *
 * Kept tiny on purpose: it shares the one Serial console the firmware
 * already uses for logging, so when ENABLE_USB_LINK is set the ESP32 emits
 * machine-readable one-line JSON interleaved with human logs the host agent
 * can still pick out (JSON lines are matched on "\"evt\":" tokens).
 *
 * The host also provisions the station network here: {"cmd":"net",...}
 * replaces hardcoded router credentials on the mobile unit.
 */

#include "usb_link.h"

#include <Arduino.h>
#include <string.h>

#define USB_COMMAND_LINE_MAX (USB_LINK_SSID_MAX + USB_LINK_PSK_MAX + 64)

#if ENABLE_USB_LINK
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

#ifndef ENABLE_USB_LINK
#define ENABLE_USB_LINK 0
#endif

static usb_ap_cmd_cb_t g_ap_cmd_cb = NULL;
static usb_net_cb_t   g_net_cb = NULL;
static char g_line[USB_COMMAND_LINE_MAX];
static size_t g_line_len = 0;

#if ENABLE_USB_LINK
static char g_net_ssid[USB_LINK_SSID_MAX + 1];
static char g_net_psk[USB_LINK_PSK_MAX + 1];
static bool g_net_set = false;
static SemaphoreHandle_t g_net_lock = NULL;
#endif

static void set_net(const char *ssid, const char *psk) {
#if ENABLE_USB_LINK
    if (g_net_lock) xSemaphoreTake(g_net_lock, portMAX_DELAY);
    strncpy(g_net_ssid, ssid, USB_LINK_SSID_MAX);
    g_net_ssid[USB_LINK_SSID_MAX] = '\0';
    strncpy(g_net_psk, psk, USB_LINK_PSK_MAX);
    g_net_psk[USB_LINK_PSK_MAX] = '\0';
    g_net_set = true;
    if (g_net_lock) xSemaphoreGive(g_net_lock);
    Serial.printf("[USB] provisioned net: %s\n", g_net_ssid);

    if (g_net_cb) {
        char s[USB_LINK_SSID_MAX + 1];
        char p[USB_LINK_PSK_MAX + 1];
        if (g_net_lock) xSemaphoreTake(g_net_lock, portMAX_DELAY);
        strncpy(s, g_net_ssid, sizeof(s) - 1); s[sizeof(s) - 1] = '\0';
        strncpy(p, g_net_psk, sizeof(p) - 1); p[sizeof(p) - 1] = '\0';
        if (g_net_lock) xSemaphoreGive(g_net_lock);
        g_net_cb(s, p);
    }
#else
    (void)ssid;
    (void)psk;
#endif
}

void usb_link_init(void) {
#if ENABLE_USB_LINK
    g_line_len = 0;
    g_net_set = false;
    g_net_lock = xSemaphoreCreateMutex();
    usb_link_report("BOOT", 0);
#else
    (void)0;
#endif
}

void usb_link_report(const char *evt, int rssi) {
#if ENABLE_USB_LINK
    Serial.printf("{\"evt\":\"%s\",\"rssi\":%d,\"t\":%lu}\n",
                  evt, rssi, (unsigned long)millis());
#else
    (void)evt;
    (void)rssi;
#endif
}

void usb_link_set_ap_cmd_cb(usb_ap_cmd_cb_t cb) {
    g_ap_cmd_cb = cb;
}

void usb_link_set_net_cb(usb_net_cb_t cb) {
    g_net_cb = cb;
}

bool usb_link_get_net(char *ssid, size_t ssid_sz, char *psk, size_t psk_sz) {
#if ENABLE_USB_LINK
    bool ok = false;
    if (!ssid || !psk) return false;

    if (g_net_lock) xSemaphoreTake(g_net_lock, portMAX_DELAY);
    ok = g_net_set;
    if (ok) {
        snprintf(ssid, ssid_sz, "%s", g_net_ssid);
        snprintf(psk, psk_sz, "%s", g_net_psk);
    }
    if (g_net_lock) xSemaphoreGive(g_net_lock);
    return ok;
#else
    (void)ssid;
    (void)ssid_sz;
    (void)psk;
    (void)psk_sz;
    return false;
#endif
}

void usb_link_poll(void) {
#if ENABLE_USB_LINK
    while (Serial.available() > 0) {
        char c = (char)Serial.read();

        if (c == '\n' || c == '\r') {
            if (g_line_len == 0) continue;

            g_line[g_line_len] = '\0';
            g_line_len = 0;

            if (strstr(g_line, "\"cmd\":\"net\"")) {
                char *ss = strstr(g_line, "\"ssid\":\"");
                char *ps = strstr(g_line, "\"psk\":\"");
                char net_ssid[USB_LINK_SSID_MAX + 1];
                char net_psk[USB_LINK_PSK_MAX + 1];
                bool have_ssid = false, have_psk = false;

                if (ss && ps) {
                    char *s = ss + strlen("\"ssid\":\"");
                    char *e = strchr(s, '"');
                    if (e && e > s) {
                        size_t n = (size_t)(e - s);
                        if (n > USB_LINK_SSID_MAX) n = USB_LINK_SSID_MAX;
                        memcpy(net_ssid, s, n);
                        net_ssid[n] = '\0';
                        have_ssid = true;
                    }
                    char *p = ps + strlen("\"psk\":\"");
                    char *q = strchr(p, '"');
                    if (q && q > p) {
                        size_t n = (size_t)(q - p);
                        if (n > USB_LINK_PSK_MAX) n = USB_LINK_PSK_MAX;
                        memcpy(net_psk, p, n);
                        net_psk[n] = '\0';
                        have_psk = true;
                    }
                }
                if (have_ssid && have_psk) {
                    set_net(net_ssid, net_psk);
                } else {
                    Serial.println("[USB] malformed net cmd");
                }
            } else if (strstr(g_line, "\"cmd\":\"ap\"") && g_ap_cmd_cb) {
                bool up = strstr(g_line, "\"state\":1") != NULL;
                Serial.printf("[USB] host cmd: ap %s\n", up ? "up" : "down");
                g_ap_cmd_cb(up);
            }
        } else if (g_line_len < sizeof(g_line) - 1) {
            g_line[g_line_len++] = c;
        } else {
            g_line_len = 0; /* line too long - drop */
        }
    }
#else
    (void)0;
#endif
}