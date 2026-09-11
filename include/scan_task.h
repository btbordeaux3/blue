#ifndef BASE_SCAN_TASK_H
#define BASE_SCAN_TASK_H

#include <Arduino.h>
#include "common.h"
#include "rf_fingerprint.h"

void scan_task_init(void);
void scan_task_start(void);
bool scan_task_get_fingerprint(rf_fingerprint_t *fp);

#endif
