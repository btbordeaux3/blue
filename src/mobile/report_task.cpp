/*
 * report_task.cpp - Mobile unit report task with offline buffering
 *
 * Sends RF fingerprint reports to base via ESPNOW. If ESPNOW is
 * unavailable (e.g., during light sleep or weak signal), reports
 * are buffered in a ring buffer (32 slots) for later transmission.
 *
 * Interview talking points:
 *   - Circular buffer with mutex for thread-safe producer/consumer
 *   - "Store and forward" pattern: buffer data during disconnection,
 *     flush on reconnect (base sends CMD_SEND_BUFFERED)
 *   - Sequence numbers enable duplicate detection at base
 *   - 100ms inter-message delay prevents ESPNOW queue overflow
 */

#include "report_task.h"
#include "espnow_comm.h"
#include "rf_fingerprint.h"
#include "power_manager.h"
#include "common.h"

#define BUFFERED_REPORTS_MAX 32

static TaskHandle_t report_handle = NULL;
static fingerprint_msg_t report_buffer[BUFFERED_REPORTS_MAX];
static uint8_t buffer_head = 0;
static uint8_t buffer_count = 0;
static SemaphoreHandle_t buffer_mutex = NULL;
static uint8_t sequence = 0;

static bool buffer_peek(fingerprint_msg_t *msg) {
    bool ok = false;

    xSemaphoreTake(buffer_mutex, portMAX_DELAY);
    if (buffer_count > 0) {
        memcpy(msg, &report_buffer[buffer_head], sizeof(*msg));
        ok = true;
    }
    xSemaphoreGive(buffer_mutex);
    return ok;
}

static void buffer_pop(void) {
    xSemaphoreTake(buffer_mutex, portMAX_DELAY);
    if (buffer_count > 0) {
        buffer_head = (uint8_t)((buffer_head + 1) % BUFFERED_REPORTS_MAX);
        buffer_count--;
    }
    xSemaphoreGive(buffer_mutex);
}

static bool send_buffered_reports(void) {
    fingerprint_msg_t msg;
    uint8_t sent = 0;

    while (buffer_peek(&msg)) {
        if (!espnow_send_fingerprint(&msg))
            break;
        buffer_pop();
        sent++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return sent > 0;
}

static void report_task(void *param) {
    Serial.println("[REPORT] Task started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (send_buffered_reports()) {
            Serial.println("[REPORT] Sent buffered reports");
        }
    }
}

void report_task_init(void) {
    buffer_mutex = xSemaphoreCreateMutex();
}

void report_task_start(void) {
    xTaskCreatePinnedToCore(report_task, "report", 4096, NULL, 3, &report_handle, 0);
}

bool report_task_send_fingerprint(const rf_fingerprint_t *fp) {
    fingerprint_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    sequence++;
    rf_fingerprint_to_msg(fp, 0, power_manager_get_state(), &msg);
    msg.sequence_num = sequence;
    msg.activation_source = power_manager_get_activation_source();
    msg.confidence_pct = power_manager_get_activation_confidence();

    if (espnow_send_fingerprint(&msg)) {
        Serial.printf("[REPORT] Sent fingerprint (seq %u)\n", sequence);
        return true;
    }

    Serial.println("[REPORT] Send failed - buffering");
    xSemaphoreTake(buffer_mutex, portMAX_DELAY);
    if (buffer_count < BUFFERED_REPORTS_MAX) {
        uint8_t idx = (buffer_head + buffer_count) % BUFFERED_REPORTS_MAX;
        memcpy(&report_buffer[idx], &msg, sizeof(fingerprint_msg_t));
        buffer_count++;
    }
    xSemaphoreGive(buffer_mutex);
    return false;
}

void report_task_flush_buffer(void) {
    uint8_t before = report_task_get_buffered_count();
    bool sent = send_buffered_reports();
    uint8_t after = report_task_get_buffered_count();

    Serial.printf("[REPORT] Buffer flush complete (%u sent, %u remaining)%s\n",
                  (uint8_t)(before - after), after,
                  sent ? "" : " - link unavailable");
}

uint8_t report_task_get_buffered_count(void) {
    xSemaphoreTake(buffer_mutex, portMAX_DELAY);
    uint8_t count = buffer_count;
    xSemaphoreGive(buffer_mutex);
    return count;
}
