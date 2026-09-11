/*
 * config.c - Config loading/parsing.
 *
 * GKeyFile handles "key = value" INI-style files with [sections]; values can
 * be pulled from environment variables first so a dev box can override
 * without touching root-owned files (e.g. ESP_WIFI_AGENT_SSID).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "esp_wifi_agent/config.h"

static char *env_or_def(const char *env, const char *def)
{
    const char *v = env ? g_getenv(env) : NULL;
    return g_strdup(v ? v : def);
}

esp_agent_config_t *esp_agent_config_load(const char *path)
{
    esp_agent_config_t *cfg = g_new0(esp_agent_config_t, 1);
    GKeyFile           *kf  = g_key_file_new();
    GError             *err = NULL;

    if (path && g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
        /* parse ok */
    } else if (err) {
        g_printerr("[esp-wifi-agent] config %s: %s\n", path ? path : "(none)",
                   err->message);
        g_error_free(err);
        err = NULL;
    }

    /* g_key_file_get_string() returns heap memory. Keep the parsed value
     * alive only long enough to select the environment override, then let
     * env_or_def() create the owned config string. */
    {
        char *value = g_key_file_get_string(kf, "esp32", "ext_ssid", NULL);
        cfg->ext_ssid = env_or_def("ESP_WIFI_AGENT_SSID",
                          value ? value : "3bbo_Ext");
        g_free(value);
    }
    {
        char *value = g_key_file_get_string(kf, "esp32", "ext_psk", NULL);
        cfg->ext_psk = env_or_def("ESP_WIFI_AGENT_PSK",                          value ? value : "YOUR_EXTENDER_PASSWORD");
        g_free(value);
    }
    {
        char *value = g_key_file_get_string(kf, "wifi", "iface", NULL);
        cfg->wifi_iface = env_or_def("ESP_WIFI_AGENT_IFACE",
                             value ? value : "wlan0");
        g_free(value);
    }

    cfg->scan_interval_sec = g_key_file_get_integer(kf, "daemon", "scan_interval_sec", NULL);
    if (cfg->scan_interval_sec < 1) cfg->scan_interval_sec = 5;

    cfg->req_seen   = g_key_file_get_integer(kf, "daemon", "req_seen", NULL);
    if (cfg->req_seen < 1) cfg->req_seen = 2;
    cfg->req_missed = g_key_file_get_integer(kf, "daemon", "req_missed", NULL);
    if (cfg->req_missed < 1) cfg->req_missed = 2;

    {
        char *value = g_key_file_get_string(kf, "daemon", "state_file", NULL);
        char *fallback = value ? NULL : g_strdup_printf(
            "%s/.local/state/esp-wifi-agent/previous.txt", g_get_home_dir());
        cfg->state_file = env_or_def("ESP_WIFI_AGENT_STATE_FILE",
                             value ? value : fallback);
        g_free(value);
        g_free(fallback);
    }

    {
        char *value = g_key_file_get_string(kf, "usb", "serial_device", NULL);
        cfg->serial_device = env_or_def("ESP_WIFI_AGENT_SERIAL",
                                value ? value : "/dev/ttyUSB0");
        g_free(value);
    }
    cfg->serial_baud      = g_key_file_get_integer(kf, "usb", "serial_baud", NULL);
    if (cfg->serial_baud <= 0) cfg->serial_baud = 115200;
    cfg->usb_telemetry = g_key_file_get_boolean(kf, "usb", "telemetry", NULL);
    cfg->usb_command   = g_key_file_get_boolean(kf, "usb", "command", NULL);

    {
        char *value = g_key_file_get_string(kf, "kernel", "publish_device", NULL);
        cfg->publish_device = env_or_def("ESP_WIFI_AGENT_KDEVICE",
                                 value ? value : "/dev/espnet");
        g_free(value);
    }
    cfg->log_domain = g_strdup("esp-wifi-agent");

    g_key_file_free(kf);
    return cfg;
}

void esp_agent_config_free(esp_agent_config_t *cfg)
{
    if (!cfg) return;
    g_free(cfg->ext_ssid);
    g_free(cfg->ext_psk);
    g_free(cfg->wifi_iface);
    g_free(cfg->state_file);
    g_free(cfg->serial_device);
    g_free(cfg->publish_device);
    g_free(cfg->log_domain);
    g_free(cfg);
}