/*
 * state.h - Persistent storage of the "previous network" so we can restore
 * it after the ESP32 extender AP goes away. Survival across daemon restarts.
 */

#ifndef ESP_WIFI_AGENT_STATE_H
#define ESP_WIFI_AGENT_STATE_H

#include <stdbool.h>

#define ESP_STATE_MAX_NAME 128

/* Returns a heap string or NULL. Caller frees. */
char *esp_state_load(const char *path);

/* Returns true on success (atomic write: temp file + rename). */
bool esp_state_save(const char *path, const char *connection_name);

#endif /* ESP_WIFI_AGENT_STATE_H */