/*
 * usb_monitor.c - Reader thread for the ESP32 USB/UART telemetry link.
 *
 * Modeled on how a real BSP agent consumes a co-processor: a small
 * line-oriented reader that recognizes { "evt": "AP_UP" | "AP_DOWN" }
 * markers and latches them for the main loop to consume.
 */

#define _DEFAULT_SOURCE /* cfmakeraw on glibc */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <glib.h>

#include "esp_wifi_agent/usb_monitor.h"

#define USB_SSID_MAX 32
#define USB_PSK_MAX  63
#define USB_LINE_MAX (USB_SSID_MAX + USB_PSK_MAX + 64)

struct esp_usb_monitor {
    const esp_agent_config_t *cfg;
    GThread                  *thread;
    GMutex                    lock;
    int                       fd;
    bool                      running;
    bool                      latch_ap_up;
    bool                      latch_ap_down;
    bool                      latch_boot;
    bool                      ap_level_known;
    bool                      ap_level;
};

static speed_t serial_speed(int baud)
{
    switch (baud) {
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
#ifdef B230400
    case 230400: return B230400;
#endif
    default:     return 0;
    }
}

static bool write_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            g_usleep(1000);
            continue;
        }
        return false;
    }
    return true;
}

static void *usb_reader(void *user_data);

esp_usb_monitor_t *esp_usb_monitor_start(const esp_agent_config_t *cfg)
{
    esp_usb_monitor_t *u;

    if (!cfg->usb_telemetry || !cfg->serial_device || !*cfg->serial_device) {
        g_info("[usb] telemetry disabled");
        return NULL;
    }

    u = g_new0(esp_usb_monitor_t, 1);
    u->cfg     = cfg;
    u->fd      = -1;
    u->running = true;
    g_mutex_init(&u->lock);

    u->thread = g_thread_new("usb-telemetry", usb_reader, u);
    return u;
}

#define ESPNET_USB_RETRY_MS 3000

static void *usb_reader(void *user_data)
{
    esp_usb_monitor_t *u = user_data;
    struct termios conf;
    char  buf[USB_LINE_MAX];

    /* The owner initializes running before creating the thread. Do not set it
     * here: free() may request shutdown before this thread is scheduled. */
    g_mutex_lock(&u->lock);
    bool initially_running = u->running;
    g_mutex_unlock(&u->lock);
    if (!initially_running)
        return NULL;

    /*
     * Self-healing: the ESP32 is a tethered, repluggable node, so a lost or
     * missing serial port is the norm, not an error. Keep trying to open the
     * device (recovering any unplug/replug/reboot blip) instead of giving up
     * and leaving the agent blind until it is restarted.
     */
    for (;;) {
        ssize_t n;
        size_t stash = 0;
        bool    reconnected = false;

        int fd;
        bool quit;

        g_mutex_lock(&u->lock);
        quit = !u->running;
        fd = quit ? -1 : open(u->cfg->serial_device,
                              O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd >= 0)
            u->fd = fd;
        g_mutex_unlock(&u->lock);

        if (quit)
            break;
        if (fd < 0) {
            g_mutex_lock(&u->lock);
            bool quit = !u->running;
            g_mutex_unlock(&u->lock);
            if (quit) break;
            g_warning("[usb] %s not present (%s), retrying in %d s",
                      u->cfg->serial_device, g_strerror(errno),
                      ESPNET_USB_RETRY_MS / 1000);
            g_usleep(ESPNET_USB_RETRY_MS * 1000);
            continue;
        }

        speed_t speed = serial_speed(u->cfg->serial_baud);
        memset(&conf, 0, sizeof(conf));
        if (tcgetattr(fd, &conf) == 0 && speed != 0) {
            cfmakeraw(&conf);
            cfsetispeed(&conf, speed);
            cfsetospeed(&conf, speed);
            conf.c_cflag |= CLOCAL | CREAD;
            tcsetattr(u->fd, TCSANOW, &conf);
        } else if (speed == 0) {
            g_warning("[usb] unsupported serial baud %d", u->cfg->serial_baud);
        }

        g_message("[usb] reading telemetry from %s @ %d baud (%s)",
                  u->cfg->serial_device, u->cfg->serial_baud,
                  reconnected ? "reconnected" : "fresh");
        reconnected = true;

        for (;;) {
        fd_set set;
        struct timeval tv = { 1, 0 };
        struct stat a, b;

        /* USB serial is node-churn: an unplug/replug swaps the udev node
         * (new inode) but the stale fd never signals an error. Detect the
         * swap directly every tick and fall through to reconnect. */
        if (stat(u->cfg->serial_device, &a) != 0 ||
            fstat(fd, &b) != 0 || a.st_ino != b.st_ino) {
            g_mutex_lock(&u->lock);
            u->ap_level_known = false;
            g_mutex_unlock(&u->lock);
            g_warning("[usb] serial node %s went away or was replaced",
                      u->cfg->serial_device);
            break;
        }

        FD_ZERO(&set);
        FD_SET(fd, &set);
        int rc = select(fd + 1, &set, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) continue;

        n = read(fd, buf + stash, sizeof(buf) - 1 - stash);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            g_mutex_lock(&u->lock);
            u->ap_level_known = false;
            g_mutex_unlock(&u->lock);
            break;
        }
        stash += (size_t)n;
        buf[stash] = '\0';

        /* flush complete lines */
        for (;;) {
            char *nl = strchr(buf, '\n');
            if (!nl) break;
            *nl = '\0';

            if (strstr(buf, "\"evt\":") && strstr(buf, "AP_UP")) {
                g_mutex_lock(&u->lock);
                u->latch_ap_up = true;
                u->ap_level_known = true;
                u->ap_level = true;
                g_mutex_unlock(&u->lock);
            } else if (strstr(buf, "\"evt\":") && strstr(buf, "AP_DOWN")) {
                g_mutex_lock(&u->lock);
                u->latch_ap_down = true;
                u->ap_level_known = true;
                u->ap_level = false;
                g_mutex_unlock(&u->lock);
            } else if (strstr(buf, "\"evt\":") && strstr(buf, "BOOT")) {
                g_mutex_lock(&u->lock);
                u->latch_boot = true;
                g_mutex_unlock(&u->lock);
            }

            g_debug("[usb] rx: %s", buf);
            memmove(buf, nl + 1, stash - ((size_t)(nl - buf) + 1));
            stash -= (size_t)(nl - buf) + 1;
            buf[stash] = '\0';
        }
        if (stash >= sizeof(buf) - 1)
            stash = 0;             /* drop pathological partial line */
        }

        g_mutex_lock(&u->lock);
        bool owned = (u->fd == fd);
        quit = !u->running;
        if (owned)
            u->fd = -1;
        g_mutex_unlock(&u->lock);
        if (owned)
            close(fd);

        if (quit)
            break;
        g_warning("[usb] lost serial on %s, reopening in %d s",
                  u->cfg->serial_device, ESPNET_USB_RETRY_MS / 1000);
        g_usleep(ESPNET_USB_RETRY_MS * 1000);
    }

    g_mutex_lock(&u->lock);
    u->running = false;
    g_mutex_unlock(&u->lock);
    return NULL;
}

static void esp_usb_monitor_stop(esp_usb_monitor_t *u)
{
    g_mutex_lock(&u->lock);
    u->running = false;
    if (u->thread && u->fd >= 0) close(u->fd);
    g_mutex_unlock(&u->lock);

    if (u->thread) {
        g_thread_join(u->thread);
        u->thread = NULL;
    }
}

void esp_usb_monitor_free(esp_usb_monitor_t *u)
{
    if (!u) return;
    esp_usb_monitor_stop(u);
    g_mutex_clear(&u->lock);
    g_free(u);
}

bool esp_usb_consume_ap_up(esp_usb_monitor_t *u)
{
    bool v = false;
    if (!u) return false;
    g_mutex_lock(&u->lock);
    v = u->latch_ap_up;
    u->latch_ap_up = false;
    g_mutex_unlock(&u->lock);
    return v;
}

bool esp_usb_consume_ap_down(esp_usb_monitor_t *u)
{
    bool v = false;
    if (!u) return false;
    g_mutex_lock(&u->lock);
    v = u->latch_ap_down;
    u->latch_ap_down = false;
    g_mutex_unlock(&u->lock);
    return v;
}

bool esp_usb_consume_boot(esp_usb_monitor_t *u)
{
    bool v = false;
    if (!u) return false;
    g_mutex_lock(&u->lock);
    v = u->latch_boot;
    u->latch_boot = false;
    g_mutex_unlock(&u->lock);
    return v;
}

bool esp_usb_get_ap_present(esp_usb_monitor_t *u, bool *known)
{
    bool level = false;

    if (known) *known = false;
    if (!u) return false;

    g_mutex_lock(&u->lock);
    level = u->ap_level;
    if (known) *known = u->ap_level_known;
    g_mutex_unlock(&u->lock);
    return level;
}
bool esp_usb_cmd_ap(esp_usb_monitor_t *u, bool up)
{
    char msg[32];
    bool ok;

    if (!u || !u->cfg->usb_command) return false;

    g_mutex_lock(&u->lock);
    if (u->fd < 0) {
        g_mutex_unlock(&u->lock);
        return false;
    }
    snprintf(msg, sizeof(msg), "{\"cmd\":\"ap\",\"state\":%d}\n", up ? 1 : 0);
    ok = write_all(u->fd, msg, strlen(msg));
    g_mutex_unlock(&u->lock);
    g_info("[usb] tx: %s", msg);
    return ok;
}

bool esp_usb_send_net(esp_usb_monitor_t *u, const char *ssid, const char *psk)
{
    char  msg[USB_LINE_MAX];
    bool ok;

    if (!u || !u->cfg->usb_command || !ssid || !*ssid || !psk || !*psk) return false;

    g_mutex_lock(&u->lock);
    if (u->fd < 0) {
        g_mutex_unlock(&u->lock);
        return false;
    }
    snprintf(msg, sizeof(msg), "{\"cmd\":\"net\",\"ssid\":\"%s\",\"psk\":\"%s\"}\n",
             ssid, psk);
    ok = write_all(u->fd, msg, strlen(msg));
    g_mutex_unlock(&u->lock);
    g_info("[usb] tx: net ssid=%s", ssid);
    return ok;
}