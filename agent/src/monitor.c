/*
 * monitor.c - libnm-driven NetworkManager shim.
 *
 * Uses the official NetworkManager client library (libnm over D-Bus) so the
 * daemon reacts to what NM actually exposes, instead of st apple nmcli.
 * Connection activation uses AddAndActivate2 semantics: if no saved profile
 * for the extender exists yet, one is created on the fly from cfg.
 */

#include <string.h>
#include <stdio.h>

#include <NetworkManager.h>
#include <glib.h>

#include "esp_wifi_agent/monitor.h"

struct esp_monitor {
    const esp_agent_config_t *cfg;
    NMClient                 *client;
    NMDeviceWifi             *wifi_dev;
    NMDevice                 *nm_dev;      /* generic device handle */
    guint                     scan_gen;
    bool                      scan_inflight;
};

/* NetworkManager can report no device while Wi-Fi is being unplugged,
 * powered down, or brought back by the kernel. Refresh the borrowed device
 * handle on every operation instead of making a transient absence fatal. */
static bool refresh_wifi_device(esp_monitor_t *m)
{
    NMDevice *dev;

    if (!m || !m->client || !m->cfg || !m->cfg->wifi_iface)
        return false;

    dev = nm_client_get_device_by_iface(m->client, m->cfg->wifi_iface);
    if (!dev || !NM_IS_DEVICE_WIFI(dev)) {
        m->nm_dev = NULL;
        m->wifi_dev = NULL;
        return false;
    }

    m->nm_dev = dev;
    m->wifi_dev = NM_DEVICE_WIFI(dev);
    return true;
}

/* ------------------------------------------------------------------ */
static bool nm_wifi_has_ssid(NMDeviceWifi *dev, const char *ssid)
{
    const GPtrArray *aps;
    guint     i;

    if (!ssid) return false;
    aps = nm_device_wifi_get_access_points(dev);
    if (!aps) return false;

    for (i = 0; i < aps->len; i++) {
        GBytes *b = nm_access_point_get_ssid((NMAccessPoint *)aps->pdata[i]);
        if (b) {
            gsize len;
            const char *a = g_bytes_get_data(b, &len);
            if (a && strlen(ssid) == len && memcmp(a, ssid, len) == 0)
                return true;
        }
    }
    return false;
}

static NMConnection *find_conn_by_id(NMClient *client, const char *id)
{
    const GPtrArray *conns;
    guint            i;

    if (!id) return NULL;
    conns = nm_client_get_connections(client);
    for (i = 0; conns && i < conns->len; i++) {
        NMConnection *conn = NM_CONNECTION(g_ptr_array_index(conns, i));
        const char   *cid  = nm_connection_get_id(conn);
        if (cid && g_str_equal(cid, id))
            return conn;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
esp_monitor_t *esp_monitor_new(const esp_agent_config_t *cfg, GError **err)
{
    esp_monitor_t *m  = g_new0(esp_monitor_t, 1);
    NMDevice      *dev;

    m->cfg = cfg;
    m->client = nm_client_new(NULL, err);
    if (!m->client) {
        g_free(m);
        return NULL;
    }

    /* The interface may be absent or temporarily unavailable at boot. Keep
     * the monitor alive and let refresh_wifi_device() recover it later. */
    dev = nm_client_get_device_by_iface(m->client, cfg->wifi_iface);
    if (dev && NM_IS_DEVICE_WIFI(dev)) {
        m->nm_dev   = dev;
        m->wifi_dev = NM_DEVICE_WIFI(dev);
    }
    return m;
}

void esp_monitor_free(esp_monitor_t *m)
{
    if (!m) return;
    if (m->client) g_clear_object(&m->client);
    g_free(m);
}

gboolean esp_monitor_request_scan(esp_monitor_t *m)
{
    GError *err = NULL;
    if (!refresh_wifi_device(m)) return FALSE;

    if (m->scan_inflight)
        return TRUE;              /* still waiting on previous scan */

    m->scan_inflight = TRUE;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    /* deprecated in favor of _async; the blocking request is fine here. */
    nm_device_wifi_request_scan(m->wifi_dev, NULL, &err);
    G_GNUC_END_IGNORE_DEPRECATIONS
    m->scan_inflight = FALSE;

    if (err) {
        g_warning("[monitor] scan request failed: %s", err->message);
        g_error_free(err);
        return FALSE;
    }
    return TRUE;
}

bool esp_monitor_ext_present(esp_monitor_t *m)
{
    return refresh_wifi_device(m) &&
           nm_wifi_has_ssid(m->wifi_dev, m->cfg->ext_ssid);
}

bool esp_monitor_ext_connected(esp_monitor_t *m)
{
    char *active = esp_monitor_active_name(m);
    bool  yes    = active && g_str_equal(active, m->cfg->ext_ssid);
    g_free(active);
    return yes;
}

char *esp_monitor_active_name(esp_monitor_t *m)
{
    NMActiveConnection *ac;
    NMConnection *conn;
    const char *id;

    if (!refresh_wifi_device(m)) return NULL;
    ac = nm_device_get_active_connection(m->nm_dev);
    if (!ac) return NULL;

    conn = NM_CONNECTION(nm_active_connection_get_connection(ac));
    if (!conn) return NULL;

    id = nm_connection_get_id(conn);
    return id ? g_strdup(id) : NULL;
}

bool esp_monitor_ssid_in_scan(esp_monitor_t *m, const char *ssid)
{
    return refresh_wifi_device(m) &&
           nm_wifi_has_ssid(m->wifi_dev, ssid);
}

bool esp_monitor_has_connection(esp_monitor_t *m, const char *id)
{
    return m && find_conn_by_id(m->client, id) != NULL;
}

/* ------------------------------------------------------------------ */
/* Provisioning: pull the active connection's SSID + PSK so the ESP32 does
 * not need credentials baked into firmware. NetworkManager only hands out
 * stored secrets (nmcli --show-secrets) to authorized callers, i.e. root -
 * which is exactly how the systemd service runs. */
gboolean esp_monitor_get_active_creds(esp_monitor_t *m, char **ssid, char **psk)
{
    NMActiveConnection *ac;
    NMSettingWireless  *sw;
    GBytes             *b;
    NMSettingWirelessSecurity *sec;
    const char         *uuid, *id, *data;
    gsize               dlen;
    char               *cmd, *line = NULL;
    size_t              lcap = 0;
    ssize_t             n;
    FILE               *fp;
    char               *out_ssid = NULL, *out_psk = NULL;

    if (!ssid || !psk || !refresh_wifi_device(m)) return FALSE;
    *ssid = NULL;
    *psk  = NULL;

    ac = nm_device_get_active_connection(m->nm_dev);
    if (!ac) return FALSE;

    NMConnection *conn = NM_CONNECTION(nm_active_connection_get_connection(ac));
    if (!conn) return FALSE;

    /* Don't push while we ARE the network - ESP32 already knows that one. */
    id = nm_connection_get_id(conn);
    if (!id || g_str_equal(id, m->cfg->ext_ssid)) return FALSE;

    uuid = nm_connection_get_uuid(conn);
    if (!uuid) return FALSE;

    /* SSID: the bytes actually being broadcast, not the profile id. */
    out_ssid = g_strdup(id);
    sw = nm_connection_get_setting_wireless(conn);
    if (sw) {
        b = nm_setting_wireless_get_ssid(sw);
        if (b) {
            data = g_bytes_get_data(b, &dlen);
            if (data && dlen > 0)
                out_ssid = g_strndup(data, dlen);
        }
    }

    sec = nm_connection_get_setting_wireless_security(conn);
    if (sec) {
        /* PSK read as root via nmcli (simplest authorized secret path). */
        cmd = g_strdup_printf("nmcli -t --show-secrets connection show uuid %s",
                              uuid);
        fp  = popen(cmd, "r");
        g_free(cmd);
        if (fp) {
            while ((n = getline(&line, &lcap, fp)) != -1) {
                if (g_str_has_prefix(line, "802-11-wireless-security.psk:") &&
                    n > (ssize_t)strlen("802-11-wireless-security.psk:")) {
                    char *v = line + strlen("802-11-wireless-security.psk:");
                    g_strchomp(v);
                    if (*v) out_psk = g_strdup(v);
                    break;
                }
            }
            pclose(fp);
            g_free(line);
        }
    }

    if (!out_psk) {
        g_free(out_ssid);
        return FALSE;
    }

    *ssid = out_ssid;
    *psk  = out_psk;
    return TRUE;
}

/* ------------------------------------------------------------------ */
static NMConnection *build_ext_connection(const esp_agent_config_t *cfg)
{
    NMConnection *conn = NM_CONNECTION(nm_simple_connection_new());
    NMSettingConnection *s_con;
    NMSettingWireless    *s_wifi;
    NMSettingWirelessSecurity *s_sec;
    const char *id = cfg->ext_ssid;

    s_con = NM_SETTING_CONNECTION(nm_setting_connection_new());
    g_object_set(s_con,
                 NM_SETTING_CONNECTION_ID, id,
                 NM_SETTING_CONNECTION_UUID, nm_utils_uuid_generate(),
                 NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRELESS_SETTING_NAME,
                 NULL);
    nm_connection_add_setting(conn, NM_SETTING(s_con));

    s_wifi = NM_SETTING_WIRELESS(nm_setting_wireless_new());
    /* NM_SETTING_WIRELESS_SSID is a GBytes property, not a C string;
     * passing g_object_set a char* makes libnm build a malformed connection
     * ("valid UTF-8 SSID is required") which crashes NM's async path. */
    GBytes *ssid = g_bytes_new(id, strlen(id));
    g_object_set(s_wifi,
                 NM_SETTING_WIRELESS_SSID, ssid,
                 NM_SETTING_WIRELESS_MODE, NM_SETTING_WIRELESS_MODE_INFRA,
                 NM_SETTING_WIRELESS_HIDDEN, FALSE,
                 NULL);
    g_bytes_unref(ssid);
    nm_connection_add_setting(conn, NM_SETTING(s_wifi));

    s_sec = NM_SETTING_WIRELESS_SECURITY(nm_setting_wireless_security_new());
    g_object_set(s_sec,
                 NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-psk",
                 NM_SETTING_WIRELESS_SECURITY_PSK, cfg->ext_psk,
                 NULL);
    nm_connection_add_setting(conn, NM_SETTING(s_sec));

    return conn;
}

static void on_activate_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    NMClient *client = NM_CLIENT(src);
    GError   *err    = NULL;
    char     *what   = (char *)user_data;

    if (!nm_client_add_and_activate_connection_finish(client, res, &err)) {
        g_warning("[monitor] add+activate %s failed: %s",
                  what ? what : "?", err ? err->message : "(null)");
        g_clear_error(&err);
    } else {
        g_message("[monitor] activated %s", what ? what : "?");
    }
    g_free(what);
}

static void on_activate_conn_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    NMClient *client = NM_CLIENT(src);
    GError   *err    = NULL;
    char     *what   = (char *)user_data;

    if (!nm_client_activate_connection_finish(client, res, &err)) {
        g_warning("[monitor] activate %s failed: %s",
                  what ? what : "?", err ? err->message : "(null)");
        g_clear_error(&err);
    } else {
        g_message("[monitor] activated %s", what ? what : "?");
    }
    g_free(what);
}

void esp_monitor_connect_ext(esp_monitor_t *m)
{
    NMConnection *conn;

    if (!refresh_wifi_device(m)) {
        g_info("[monitor] Wi-Fi interface %s is unavailable; will retry",
               m && m->cfg ? m->cfg->wifi_iface : "(unknown)");
        return;
    }

    conn = find_conn_by_id(m->client, m->cfg->ext_ssid);
    if (conn) {
        nm_client_activate_connection_async(m->client, conn, m->nm_dev, NULL,
                                            NULL, on_activate_conn_done,
                                            g_strdup(m->cfg->ext_ssid));
    } else {
        conn = build_ext_connection(m->cfg);
        nm_client_add_and_activate_connection_async(m->client, conn, m->nm_dev,
                                                    NULL, NULL, on_activate_done,
                                                    g_strdup(m->cfg->ext_ssid));
        g_object_unref(conn);
    }
}

void esp_monitor_connect_by_id(esp_monitor_t *m, const char *id)
{
    NMConnection *conn;

    if (!refresh_wifi_device(m)) {
        g_info("[monitor] Wi-Fi interface is unavailable; will retry restore");
        return;
    }
    conn = find_conn_by_id(m->client, id);
    if (!conn) {
        g_warning("[monitor] no saved connection named '%s'", id);
        return;
    }
    nm_client_activate_connection_async(m->client, conn, m->nm_dev, NULL,
                                        NULL, on_activate_conn_done,
                                        g_strdup(id));
}

void esp_monitor_disconnect_active(esp_monitor_t *m)
{
    NMActiveConnection *ac = refresh_wifi_device(m) && m->nm_dev
        ? nm_device_get_active_connection(m->nm_dev) : NULL;

    if (!ac) return;
    nm_client_deactivate_connection_async(m->client, ac, NULL, NULL, NULL);
}