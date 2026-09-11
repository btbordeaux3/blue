#ifndef MOBILE_REPEATER_TASK_H
#define MOBILE_REPEATER_TASK_H

#include <Arduino.h>
#include "common.h"

void repeater_task_init(void);

/* Refresh the station (router) credentials from the host-provisioned values.
 * Call at boot and whenever the host provisions a new network over USB-C. */
void repeater_task_set_credentials(const char *ssid, const char *psk);

/* Copy the current station (router) credentials. Callers that need to
 * (re)join the host network - e.g. after a scan, which drops the link -
 * must use these instead of the compile-time defaults so a USB-provisioned
 * network takes effect everywhere. */
void repeater_task_get_credentials(char *ssid, size_t ssid_sz, char *psk, size_t psk_sz);

void repeater_task_start(void);
void repeater_task_activate(void);
void repeater_task_deactivate(void);
bool repeater_task_is_active(void);
uint8_t repeater_task_get_client_count(void);

#endif
