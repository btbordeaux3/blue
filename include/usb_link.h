/*
 * usb_link.h - Wired (USB-C serial) link between the mobile ESP32 and the
 * Linux host that powers it.
 *
 * The mobile unit is tethered over USB-C: the host supplies 5V and reads
 * the ESP32 console (UART0 -> USB-UART bridge -> /dev/ttyUSB0). This module
 * emits one-line JSON events so the host's esp-wifi-agent can react to the
 * softAP turning on/off WITHOUT waiting for a wifi scan:
 *
 *   {"evt":"AP_UP","rssi":-72,"t":1234}
 *   {"evt":"AP_DOWN","rssi":0,"t":5678}
 *   {"evt":"BOOT","rssi":0,"t":0}          <- lets host (re)provision creds
 *
 * It also accepts host commands on the same link:
 *   {"cmd":"ap","state":1}   -> bring the extender AP up
 *   {"cmd":"ap","state":0}   -> drop it
 *   {"cmd":"net","ssid":"...","psk":"..."} -> the network to join as a
 *        station. This is the zero-config provisioning path: the host tells
 *        the mobile which Wi-Fi to use, so firmware has no credentials
 *        baked in.
 *
 * Guarded by ENABLE_USB_LINK so base builds stay inert.
 */

#ifndef BLUE_USB_LINK_H
#define BLUE_USB_LINK_H

#include <stdbool.h>
#include <stddef.h>

#define USB_LINK_SSID_MAX 32
#define USB_LINK_PSK_MAX  63

#ifdef __cplusplus
extern "C" {
#endif

/* Sets up the link and emits a BOOT event. Cheap; call in setup(). */
void usb_link_init(void);

/* Emit a structured event line, e.g. usb_link_report("AP_UP", -72). */
void usb_link_report(const char *evt, int rssi);

/* Register the handler for {"cmd":"ap",...} host commands. */
typedef void (*usb_ap_cmd_cb_t)(bool up);
void usb_link_set_ap_cmd_cb(usb_ap_cmd_cb_t cb);

/* Register the handler for {"cmd":"net",...} provisioning: called with the
 * new network the host wants the mobile to join as a station. */
typedef void (*usb_net_cb_t)(const char *ssid, const char *psk);
void usb_link_set_net_cb(usb_net_cb_t cb);

/* Drain + parse incoming host commands; call periodically from loop(). */
void usb_link_poll(void);

/* Copy the host-provisioned network creds, if any. Returns true when set;
 * otherwise callers should fall back to their compiled defaults. */
bool usb_link_get_net(char *ssid, size_t ssid_sz, char *psk, size_t psk_sz);

#ifdef __cplusplus
}
#endif

#endif /* BLUE_USB_LINK_H */