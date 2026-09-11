/*
 * main.c - esp-wifi-agent: ESP32 <-> WiFi handoff daemon.
 *
 * Loop (per scan interval):
 *   1. request a wifi scan (NetworkManager over D-Bus)
 *   2. fuse the wifi-scan signal with the wired ESP32 telemetry signal
 *   3. run the hysteresis + decision engine
 *   4. act (connect ext / restore previous / fall back) and log the reason
 *   5. publish state into the kernel driver (/dev/espnet), best effort
 *
 * Integrates with systemd via sd_notify-compatible status + watchdog over
 * $NOTIFY_SOCKET without linking libsystemd (see notify_systemd()).
 */

#define _DEFAULT_SOURCE /* strnlen, cfmakeraw, etc. on glibc */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <glib.h>

#include "esp_wifi_agent/config.h"
#include "esp_wifi_agent/decision.h"
#include "esp_wifi_agent/espnet_ioctl.h"
#include "esp_wifi_agent/monitor.h"
#include "esp_wifi_agent/state.h"
#include "esp_wifi_agent/usb_monitor.h"

typedef struct {
    esp_agent_config_t *cfg;
    esp_monitor_t      *mon;
    esp_usb_monitor_t  *usb;
    esp_hyst_t          hyst;
    gchar              *previous;
    GMainLoop          *loop;
    /* provisioning state: last network we pushed to the ESP32 */
    gchar              *prov_ssid;
    gchar              *prov_psk;
    gchar              *last_active;
    bool                prov_pending;
    gint64              last_connect_ms;   /* monotonic ms of last CONNECT_EXT */
} app_t;

static volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_reload = 0;

#define ESP_WIFI_AGENT_VERSION "0.1.0"

static void usage(FILE *out)
{
    fprintf(out,
        "esp-wifi-agent %s - roam between the router and the 3bbo_Ext extender\n"
        "\n"
        "Usage: esp-wifi-agent [OPTIONS]\n"
        "\n"
        "  -c FILE     config file        (env ESP_WIFI_AGENT_CONFIG)\n"
        "  -v          verbose; exit on any warning (CI-friendly)\n"
        "  -V, --version   print version and exit\n"
        "  -h, --help      this help\n"
        "\n"
        "Watches NetworkManager for the ESP32 extender AP, optionally fused\n"
        "with wired USB telemetry, and hands off between home and extender.\n",
        ESP_WIFI_AGENT_VERSION);
}

/* ------------------------------------------------------------------ */
static void on_signal(int sig)
{
    if (sig == SIGHUP)
        g_reload = 1;
    else
        g_quit = 1;
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* sd_notify without libsystemd: write a datagram to $NOTIFY_SOCKET (unix). */
static void notify_systemd(const char *state)
{
    const char *sock = g_getenv("NOTIFY_SOCKET");
    struct sockaddr_un addr;
    int fd;
    size_t len;

    if (!sock || !*sock) return;

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
    len = strnlen(addr.sun_path, sizeof(addr.sun_path)) +
          offsetof(struct sockaddr_un, sun_path);
    sendto(fd, state, strlen(state), MSG_NOSIGNAL,
           (struct sockaddr *)&addr, (socklen_t)len);
    close(fd);
}

static void publish_kernel(const app_t *app, bool ap_active)
{
    int fd;

    if (!app->cfg->publish_device || !*app->cfg->publish_device) return;

    fd = open(app->cfg->publish_device, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return;                     /* module not loaded; that's fine */

    ioctl(fd, ESPNET_IOCTL_SET_AP, ap_active ? 1u : 0u);
    close(fd);
}

/* ------------------------------------------------------------------ */
static gboolean tick(gpointer user_data)
{
    app_t *app = user_data;
    esp_decision_input_t in;
    esp_agent_action_t   act;
    gboolean usb_up, usb_down;
    bool     usb_level_known;
    gboolean present_signal;
    bool     ext_connected;
    gchar   *active = NULL;

    if (g_quit) {
        g_main_loop_quit(app->loop);
        return G_SOURCE_REMOVE;
    }
    if (!app->mon) {
        GError *monitor_err = NULL;
        app->mon = esp_monitor_new(app->cfg, &monitor_err);
        if (!app->mon) {
            g_info("[main] NetworkManager unavailable; retrying: %s",
                   monitor_err ? monitor_err->message : "unknown error");
            g_clear_error(&monitor_err);
            return TRUE;
        }
        g_message("[main] NetworkManager became available; monitoring resumed");
    }
    if (g_reload) {
        g_message("[main] SIGHUP: reloading configuration");
        /* state file may have changed on disk; previous lives in memory so
           the config reload only refreshes options we don't cache here. */
        g_reload = 0;
    }

    esp_monitor_request_scan(app->mon);

    usb_up   = esp_usb_consume_ap_up(app->usb);
    usb_down = esp_usb_consume_ap_down(app->usb);
    (void)esp_usb_get_ap_present(app->usb, &usb_level_known);

    /* Wired telemetry is the strongest source of truth. Keep using the last
     * reported USB level between events; only fall back to the scan when the
     * USB device has never reported an AP state. */
    if (usb_down)
        present_signal = FALSE;
    else if (usb_up)
        present_signal = TRUE;
    else if (usb_level_known)
        present_signal = esp_usb_get_ap_present(app->usb, NULL);
    else
        present_signal = esp_monitor_ext_present(app->mon);

    esp_hyst_feed(&app->hyst, present_signal,
                  app->cfg->req_seen, app->cfg->req_missed,
                  &in.ext_confirm, &in.ext_gone);

    active = esp_monitor_active_name(app->mon);
    ext_connected = active && g_str_equal(active, app->cfg->ext_ssid);

    /* Provisioning (host -> ESP32): the mobile node learns which network to
     * join from the host over USB-C, so no credentials live in firmware.
     * Push when the AP changes or the ESP32 just rebooted (its BOOT event). */
    {
        gboolean got_boot = esp_usb_consume_boot(app->usb);
        gboolean net_changed = !app->last_active ||
            (active && !g_str_equal(active, app->last_active));

        /* The handshake is event-driven and loss-prone: a BOOT heard while
         * no usable network exists (wifi unavailable / still on the extender)
         * must not vanish - defer it and retry every tick until we can send. */
        if ((got_boot || app->prov_pending) && (!active || ext_connected))
            app->prov_pending = true;

        if (active && !ext_connected && (got_boot || app->prov_pending || net_changed)) {
            char *nssid = NULL, *npsk = NULL;
            if (esp_monitor_get_active_creds(app->mon, &nssid, &npsk)) {
                if (!app->prov_ssid || !g_str_equal(app->prov_ssid, nssid) ||
                    !app->prov_psk  || !g_str_equal(app->prov_psk,  npsk)) {
                    if (esp_usb_send_net(app->usb, nssid, npsk)) {
                        g_free(app->prov_ssid); app->prov_ssid = nssid;
                        g_free(app->prov_psk);  app->prov_psk  = npsk;
                        app->prov_pending = false;
                        g_info("[main] provisioned '%s' to ESP32", nssid);
                    } else {
                        app->prov_pending = true;
                        g_free(nssid); g_free(npsk);
                    }
                } else {
                    g_free(nssid); g_free(npsk);
                }
            } else {
                app->prov_pending = true;
                g_free(nssid); g_free(npsk);
            }
        }
        if (active && *active) {
            g_free(app->last_active);
            app->last_active = g_strdup(active);
        }
    }

    in.ext_present_signal = present_signal;
    in.ext_connected      = ext_connected;
    in.ext_profile_ready  = esp_monitor_has_connection(app->mon, app->cfg->ext_ssid);
    in.previous_known     = app->previous && *app->previous;
    in.previous_available = in.previous_known &&
                            (esp_monitor_ssid_in_scan(app->mon, app->previous) ||
                             (active && g_str_equal(active, app->previous)));

    act = esp_decision_run(&in);

    switch (act) {
    case ESP_ACT_CONNECT_EXT:
    {
        /* After an activation the WiFi scan can briefly drop the NM active
         * connection, causing esp_monitor_active_name() to return NULL for
         * one tick.  Without a cooldown the daemon immediately re-issues
         * CONNECT_EXT, creating a connect-storm.  Ignore the action if we
         * activated within the last 2 scan intervals. */
        gint64 now_ms = g_get_monotonic_time() / 1000;
        gint64 cooldown_ms = (gint64)app->cfg->scan_interval_sec * 2000;
        if (now_ms - app->last_connect_ms < cooldown_ms) {
            g_debug("[action] connect ext ssid=%s — suppressed (cooldown)",
                    app->cfg->ext_ssid);
            break;
        }
        if (active && !ext_connected && !g_str_equal(active, app->cfg->ext_ssid)) {
            g_free(app->previous);
            app->previous = g_strdup(active);
            esp_state_save(app->cfg->state_file, active);
        }
        g_message("[action] connect ext ssid=%s", app->cfg->ext_ssid);
        esp_monitor_connect_ext(app->mon);
        app->last_connect_ms = now_ms;
        break;
    }

    case ESP_ACT_RESTORE_PREVIOUS:
        g_message("[action] restore previous=%s", app->previous ? app->previous : "(none)");
        esp_monitor_disconnect_active(app->mon);
        if (app->previous)
            esp_monitor_connect_by_id(app->mon, app->previous);
        break;

    case ESP_ACT_FALLBACK:
        g_message("[action] fallback: let NetworkManager autoconnect");
        esp_monitor_disconnect_active(app->mon);
        break;

    case ESP_ACT_NONE:
    default:
        break;
    }

    publish_kernel(app, ext_connected);
    g_free(active);

    {
        gchar *status = g_strdup_printf(
            "STATUS=present=%d streak=%d ext_active=%s",
            in.ext_confirm, app->hyst.present_streak,
            ext_connected ? "yes" : "no");
        notify_systemd(status);
        g_free(status);
    }
    notify_systemd("WATCHDOG=1");

    return TRUE;
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    const char *conf_path = g_getenv("ESP_WIFI_AGENT_CONFIG");
    app_t app = {0};
    GError *err = NULL;
    guint timer_id;
    int opt;

    while (1) {
        int oi = 0;
        static const struct option lopts[] = {
            { "help",    no_argument, NULL, 'h' },
            { "version", no_argument, NULL, 'V' },
            { NULL, 0, NULL, 0 },
        };
        opt = getopt_long(argc, argv, "c:vhV", lopts, &oi);
        if (opt == -1) break;

        switch (opt) {
        case 'c': conf_path = optarg; break;
        case 'v': g_log_set_always_fatal(G_LOG_LEVEL_WARNING); break;
        case 'V': printf("esp-wifi-agent %s\n", ESP_WIFI_AGENT_VERSION);
                  return 0;
        case 'h':
        default:  usage(opt == 'h' ? stdout : stderr);
                  return opt == 'h' ? 0 : 1;
        }
    }

    install_signals();

    app.cfg = esp_agent_config_load(conf_path);
    app.loop = g_main_loop_new(NULL, FALSE);

    app.mon = esp_monitor_new(app.cfg, &err);
    if (!app.mon) {
        g_info("[main] cannot reach NetworkManager yet: %s; continuing and retrying",
               err ? err->message : "(null)");
        g_clear_error(&err);
    }

    app.usb = esp_usb_monitor_start(app.cfg);

    /* make sure the state dir exists */
    if (app.cfg->state_file) {
        gchar *dir = g_path_get_dirname(app.cfg->state_file);
        g_mkdir_with_parents(dir, 0755);
        g_free(dir);
    }
    app.previous = esp_state_load(app.cfg->state_file);
    esp_hyst_init(&app.hyst);

    timer_id = g_timeout_add_seconds(app.cfg->scan_interval_sec, tick, &app);
    (void)timer_id;

    notify_systemd("READY=1");
    g_message("[main] esp-wifi-agent up: ext=%s interval=%ds iface=%s",
              app.cfg->ext_ssid, app.cfg->scan_interval_sec,
              app.cfg->wifi_iface);

    g_main_loop_run(app.loop);

    /* graceful shutdown: drop the extender AP first if we are on it */
    {
        gchar *active = esp_monitor_active_name(app.mon);
        if (active && g_str_equal(active, app.cfg->ext_ssid))
            esp_monitor_disconnect_active(app.mon);
        g_free(active);
    }

    notify_systemd("STOPPING=1");
    esp_usb_monitor_free(app.usb);
    esp_monitor_free(app.mon);
    g_free(app.previous);
    g_free(app.prov_ssid);
    g_free(app.prov_psk);
    g_free(app.last_active);
    g_main_loop_unref(app.loop);
    esp_agent_config_free(app.cfg);
    return 0;
}