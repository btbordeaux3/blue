/*
 * usb_monitor.h - Wired (USB/UART) telemetry channel to the ESP32.
 *
 * The mobile ESP32 is tethered to the host over a USB-C cable: it is powered
 * by the host's 5V rail and its UART0 console is reachable through the
 * on-board USB-UART bridge (e.g. /dev/ttyUSB0). The firmware emits one-line
 * JSON like  {"evt":"AP_UP","rssi":-70,"t":123}  and may accept commands
 * like  {"cmd":"ap","state":1}  over the same link.
 */

#ifndef ESP_WIFI_AGENT_USB_MONITOR_H
#define ESP_WIFI_AGENT_USB_MONITOR_H

#include <stdbool.h>

#include "esp_wifi_agent/config.h"

typedef struct esp_usb_monitor esp_usb_monitor_t;

/* Starts the reader thread (no-op when usb_telemetry is disabled or the
 * device path is unset). Free with esp_usb_monitor_free(). */
esp_usb_monitor_t *esp_usb_monitor_start(const esp_agent_config_t *cfg);
void               esp_usb_monitor_free(esp_usb_monitor_t *u);

/* Consume a latched event (returns true once, then clears the latch). */
bool esp_usb_consume_ap_up(esp_usb_monitor_t *u);
bool esp_usb_consume_ap_down(esp_usb_monitor_t *u);
bool esp_usb_consume_boot(esp_usb_monitor_t *u);

/* Return the last level reported over USB. `known` is false until an AP_UP
 * or AP_DOWN event has been received, so callers can fall back to Wi-Fi scan. */
bool esp_usb_get_ap_present(esp_usb_monitor_t *u, bool *known);

/* Send a command to the ESP32 (only if usb_command is enabled). */
bool esp_usb_cmd_ap(esp_usb_monitor_t *u, bool up);

/* Provision the Wi-Fi network the ESP32 should join as a station. This is
 * how the host hands the mobile unit its credentials instead of baking them
 * into firmware ("zero-config mobile node"). */
bool esp_usb_send_net(esp_usb_monitor_t *u, const char *ssid, const char *psk);

#endif /* ESP_WIFI_AGENT_USB_MONITOR_H */