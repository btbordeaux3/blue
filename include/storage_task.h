#ifndef BASE_STORAGE_TASK_H
#define BASE_STORAGE_TASK_H

#include <Arduino.h>
#include "common.h"

void storage_task_init(void);
void storage_task_start(void);
bool storage_task_write_map_entry(const rf_map_entry_t *entry);
bool storage_task_load_map(void);
uint16_t storage_task_get_entry_count(void);

#endif
