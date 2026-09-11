#ifndef MOBILE_SCAN_TASK_H
#define MOBILE_SCAN_TASK_H

#include <Arduino.h>
#include "common.h"
#include "rf_fingerprint.h"

void mobile_scan_task_init(void);
void mobile_scan_task_start(void);
void mobile_scan_task_stop(void);
bool mobile_scan_get_fingerprint(rf_fingerprint_t *fp);

#endif
