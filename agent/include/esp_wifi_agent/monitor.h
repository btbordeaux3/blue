/*
 * monitor.h - NetworkManager client shim.
 *
 * Thin wrapper around libnm speaking over D-Bus to NetworkManager:
 *  - requests scans and reads AP visibility
 *  - activates/deactivates connections (creates the extender profile on demand)
 *  - reports the currently active connection id
 */

#ifndef ESP_WIFI_AGENT_MONITOR_H
#define ESP_WIFI_AGENT_MONITOR_H

#include <stdbool.h>

#include <glib-object.h>

#include "esp_wifi_agent/config.h"

typedef struct esp_monitor esp_monitor_t;

/* Blocks for the NM service; caller frees with esp_monitor_free(). */
esp_monitor_t *esp_monitor_new(const esp_agent_config_t *cfg, GError **err);
void           esp_monitor_free(esp_monitor_t *m);

/* Kick a scan; results are observed on the next poll of getters. */
gboolean esp_monitor_request_scan(esp_monitor_t *m);

/* True while the extender AP is in the current AP list. */
bool esp_monitor_ext_present(esp_monitor_t *m);

/* True when the active connection id == extender SSID. */
bool esp_monitor_ext_connected(esp_monitor_t *m);

/* Heap string of the active connection id (or NULL, no active conn). */
char *esp_monitor_active_name(esp_monitor_t *m);

/* Credentials of the currently active Wi-Fi connection, for provisioning the
 * mobile ESP32. Returns TRUE with heap *ssid/*psk when there is an active
 * connection and the agent is authorized to read its secrets (it is, when
 * running as root under systemd). Returns FALSE when idle or when the active
 * connection IS the extender (nothing to push - that would be circular). */
gboolean esp_monitor_get_active_creds(esp_monitor_t *m, char **ssid, char **psk);

/* True when `ssid` appears anywhere in the AP list. */
bool esp_monitor_ssid_in_scan(esp_monitor_t *m, const char *ssid);

/* True when a saved NM connection exists with this id. */
bool esp_monitor_has_connection(esp_monitor_t *m, const char *id);

/* Activate the extender profile (autocreates it from cfg if missing). */
void esp_monitor_connect_ext(esp_monitor_t *m);
void esp_monitor_connect_by_id(esp_monitor_t *m, const char *id);
void esp_monitor_disconnect_active(esp_monitor_t *m);

#endif /* ESP_WIFI_AGENT_MONITOR_H */