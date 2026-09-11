/*
 * storage_task.cpp - Persistent RF map storage using SPIFFS
 *
 * Writes rf_map_entry_t records to SPIFFS (SPI Flash File System).
 * Uses append mode to avoid rewriting the entire file on each entry.
 *
 * Interview talking points:
 *   - Queue-based writes decouple scan task from slow flash I/O
 *   - SPIFFS is wear-leveling aware (for ESP32 flash longevity)
 *   - Binary format avoids JSON parsing overhead on resource-constrained MCU
 *   - Queue depth of 50 entries provides ~30s buffer at typical scan rates
 */

#include "storage_task.h"
#include "ml_model.h"
#include <SPIFFS.h>
#include "common.h"

#define MAPStorage_FILE "/rf_map.dat"

static TaskHandle_t storage_handle = NULL;
static QueueHandle_t storage_queue = NULL;

typedef struct {
    rf_map_entry_t entry;
} storage_msg_t;

static uint16_t stored_count = 0;

/*
 * Compact rf_map.dat down to the NEWEST MAP_MAX_ENTRIES once it overflows.
 * Oldest observations are discarded (finite history) so the flash file
 * never grows without bound.
 */
static void compact_file(void) {
    File f = SPIFFS.open(MAPStorage_FILE, "r");
    if (!f) return;

    uint32_t n = f.size() / sizeof(rf_map_entry_t);
    if (n <= MAP_MAX_ENTRIES) {
        f.close();
        return;
    }

    uint32_t skip = n - MAP_MAX_ENTRIES;
    f.seek((long)skip * (long)sizeof(rf_map_entry_t));

    static rf_map_entry_t keep[MAP_MAX_ENTRIES];
    uint32_t rd = f.read((uint8_t *)keep, MAP_MAX_ENTRIES * sizeof(rf_map_entry_t));
    f.close();

    File w = SPIFFS.open(MAPStorage_FILE, "w");
    if (w) {
        w.write((const uint8_t *)keep, rd);
        w.close();
        stored_count = (uint16_t)(rd / sizeof(rf_map_entry_t));
        Serial.printf("[STORAGE] Map full - trimmed to latest %u entries\n", stored_count);
    }
}

static void storage_task(void *param) {
    Serial.println("[STORAGE] Task started");

    uint32_t writes_since_check = 0;
    while (1) {
        storage_msg_t msg;
        if (xQueueReceive(storage_queue, &msg, pdMS_TO_TICKS(5000)) == pdTRUE) {
            File f = SPIFFS.open(MAPStorage_FILE, "a");
            if (f) {
                f.write((const uint8_t *)&msg.entry, sizeof(rf_map_entry_t));
                f.close();
                stored_count++;
            } else {
                Serial.println("[STORAGE] Failed to open map file");
            }

            /* Capacity management: when the dataset is full, throw away the
             * oldest observations (FIFO), not the newest. */
            writes_since_check++;
            if (writes_since_check >= 32) {
                writes_since_check = 0;
                compact_file();
            }
        }
    }
}

void storage_task_init(void) {
    storage_queue = xQueueCreate(50, sizeof(storage_msg_t));
}

void storage_task_start(void) {
    xTaskCreatePinnedToCore(storage_task, "storage", 4096, NULL, 2, &storage_handle, 0);
}

bool storage_task_write_map_entry(const rf_map_entry_t *entry) {
    storage_msg_t msg;
    msg.entry = *entry;
    return xQueueSend(storage_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool storage_task_load_map(void) {
    File f = SPIFFS.open(MAPStorage_FILE, "r");
    if (!f) {
        Serial.println("[STORAGE] No saved map found");
        return false;
    }

    uint16_t loaded = 0;
    while (f.available() >= sizeof(rf_map_entry_t)) {
        rf_map_entry_t entry;
        f.read((uint8_t *)&entry, sizeof(rf_map_entry_t));
        rf_map_add_entry(&entry);
        loaded++;
    }
    f.close();

    Serial.printf("[STORAGE] Loaded %u map entries\n", loaded);
    stored_count = loaded;
    return true;
}

uint16_t storage_task_get_entry_count(void) {
    return stored_count;
}
