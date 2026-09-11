/*
 * espnet.h - Shared ABI between the espnet kernel modules and user space.
 *
 * The daemon (agent/) publishes "ESP32 extender AP is active" into the
 * kernel through ESPNET_IOCTL_SET_AP; any reader learns overall state via
 * ESPNET_IOCTL_GET_STATE. Userspace mirror: agent/include/esp_wifi_agent/
 * espnet_ioctl.h — keep both in sync.
 */

#ifndef _ESPNET_H
#define _ESPNET_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ESPNET_IOCTL_MAGIC 'E'

#define ESPNET_EXT_SSID_LEN 64

struct espnet_state {
	__u32 ap_active;    /* 1 = ESP32 extender AP currently active  */
	__s32 rssi;         /* last reported RSSI (dBm) or 0           */
	char  ext_ssid[ESPNET_EXT_SSID_LEN];
	__u32 vbus_power;   /* USB-C 5V rail to the ESP32 is enabled   */
};

#define ESPNET_IOCTL_GET_STATE _IOR(ESPNET_IOCTL_MAGIC, 0, struct espnet_state)
#define ESPNET_IOCTL_SET_AP    _IOW(ESPNET_IOCTL_MAGIC, 1, __u32)

#ifdef __KERNEL__
/* internal API between the espnet modules (espnet_core is the hub).
 * kernel-only: u8 / bool are not user-space types. */
int espnet_core_rx(const u8 *data, size_t len);
int espnet_core_set_ap(bool active);
#endif

#endif /* _ESPNET_H */