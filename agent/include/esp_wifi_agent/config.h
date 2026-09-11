/*
 * config.h - Daemon configuration (GKeyFile/INI parsed in config.c).
 */

#ifndef ESP_WIFI_AGENT_CONFIG_H
#define ESP_WIFI_AGENT_CONFIG_H

#include <stdbool.h>

typedef struct {
    char *ext_ssid;
    char *ext_psk;
    char *wifi_iface;

    int   scan_interval_sec;   /* seconds between scheduled scans */
    int   req_seen;            /* hysteresis: ticks to treat AP as present */
    int   req_missed;          /* hysteresis: ticks to treat AP as gone */

    char *state_file;
    char *serial_device;       /* e.g. /dev/ttyUSB0 (ESP32 over USB-UART bridge) */
    int   serial_baud;
    bool  usb_telemetry;       /* parse AP_UP/AP_DOWN JSON lines from serial */
    bool  usb_command;         /* allow host to command AP up/down over serial */
    char *publish_device;      /* optional /dev/espnet to mirror state via ioctl */

    char *log_domain;
} esp_agent_config_t;

/* Loads config. `path` may be NULL to use defaults. Returns filled struct
 * (heap strings) — free with esp_agent_config_free(). */
esp_agent_config_t *esp_agent_config_load(const char *path);
void               esp_agent_config_free(esp_agent_config_t *cfg);

#endif /* ESP_WIFI_AGENT_CONFIG_H */