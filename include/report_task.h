#ifndef MOBILE_REPORT_TASK_H
#define MOBILE_REPORT_TASK_H

#include <Arduino.h>
#include "common.h"
#include "rf_fingerprint.h"

void report_task_init(void);
void report_task_start(void);
bool report_task_send_fingerprint(const rf_fingerprint_t *fp);
void report_task_flush_buffer(void);
uint8_t report_task_get_buffered_count(void);

#endif
