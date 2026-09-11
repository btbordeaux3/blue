#ifndef MOBILE_OLED_TASK_H
#define MOBILE_OLED_TASK_H

#include <Arduino.h>
#include "common.h"

void oled_task_init(void);
void oled_task_start(void);
void oled_task_update_state(mobile_state_t state, rssi_t rssi, battery_pct_t bat);
void oled_task_update_repeater(bool active, uint8_t clients);

#endif
