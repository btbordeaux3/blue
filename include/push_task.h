#ifndef BASE_PUSH_TASK_H
#define BASE_PUSH_TASK_H

#include <Arduino.h>
#include "common.h"
#include "rf_fingerprint.h"
#include "metrics.h"

struct push_decision_t {
    uint8_t  active;          /* repeater currently on (as reported by mobile) */
    uint8_t  source;          /* activation_source_t */
    float    confidence;      /* 0..1 - model or local confidence */
    uint8_t  model_trained;
    uint16_t model_samples;
    uint16_t model_pos;
    uint16_t model_neg;
    uint8_t  mobile_state;
};

void push_task_init(const char *api_url);
void push_task_start(void);
bool push_task_send_fingerprint(const rf_fingerprint_t *fp);
bool push_task_send_status(uint32_t total_entries, uint32_t weak_count);
bool push_task_send_metrics(const metrics_snapshot_t *snap);
bool push_task_send_decision(const push_decision_t *d);

#endif
