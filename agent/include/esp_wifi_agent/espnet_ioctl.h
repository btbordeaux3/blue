/*
 * espnet_ioctl.h - Userspace side of the /dev/espnet ioctl ABI.
 *
 * MUST stay in sync with kmod/espnet.h. The kernel module is the single
 * source of truth for the wire wires it; this header exists so user space
 * (the agent) can publish reachability state into the kernel driver.
 */

#ifndef ESP_WIFI_AGENT_ESPNET_IOCTL_H
#define ESP_WIFI_AGENT_ESPNET_IOCTL_H

#include <stdint.h>
#include <sys/ioctl.h>

#define ESPNET_IOCTL_MAGIC 'E'

struct espnet_state {
    uint32_t ap_active;   /* 1 = ESP32 extender AP currently active */
    int32_t  rssi;        /* last reported RSSI (dBm) or 0 */
    char     ext_ssid[64];
    uint32_t vbus_power;  /* USB-C 5V rail enabled */
};

#define ESPNET_IOCTL_GET_STATE _IOR(ESPNET_IOCTL_MAGIC, 0, struct espnet_state)
#define ESPNET_IOCTL_SET_AP    _IOW(ESPNET_IOCTL_MAGIC, 1, uint32_t)

#endif /* ESP_WIFI_AGENT_ESPNET_IOCTL_H */