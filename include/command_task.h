#ifndef BASE_COMMAND_TASK_H
#define BASE_COMMAND_TASK_H

#include <Arduino.h>
#include "common.h"

void command_task_init(void);
void command_task_start(void);
void command_task_notify_scan_done(void);
void command_task_process_report(const fingerprint_msg_t *report, const uint8_t *mac);
connection_status_t command_task_get_connection_status(void);
uint8_t command_task_get_active_clients(void);

#endif
