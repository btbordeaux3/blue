#ifndef MOBILE_POWER_MANAGER_H
#define MOBILE_POWER_MANAGER_H

#include <Arduino.h>
#include "common.h"

void power_manager_init(void);
void power_manager_start(void);
void power_manager_set_state(mobile_state_t new_state);
mobile_state_t power_manager_get_state(void);
void power_manager_request_activate(void);
void power_manager_request_deactivate(deactivation_reason_t reason);
void power_manager_enter_sleep(uint32_t sleep_duration_ms);
void power_manager_print_status(void);
activation_source_t power_manager_get_activation_source(void);
uint8_t power_manager_get_activation_confidence(void);

#endif
