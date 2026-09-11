#include "oled_task.h"
#ifdef ENABLE_OLED
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C

static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
static SemaphoreHandle_t oled_mutex = NULL;
static TaskHandle_t oled_handle = NULL;

static mobile_state_t last_state = STATE_SLEEP;
static rssi_t last_rssi = 0;
static battery_pct_t last_bat = 100;
static bool repeater_active = false;
static uint8_t repeater_clients = 0;

static void draw_display(void) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    display.println("BLUE Mobile");

    display.setCursor(0, 12);
    display.printf("State: ");
    switch (last_state) {
        case STATE_SLEEP:     display.println("SLEEP"); break;
        case STATE_SCAN:      display.println("SCAN"); break;
        case STATE_EVALUATE:  display.println("EVAL"); break;
        case STATE_REPORT:    display.println("REPORT"); break;
        case STATE_REPEATER:  display.println("REPEATER"); break;
        case STATE_RECONNECT: display.println("RECON"); break;
        case STATE_LOST:      display.println("LOST"); break;
        default:              display.println("???"); break;
    }

    display.setCursor(0, 24);
    display.printf("RSSI: %d dBm", last_rssi);

    display.setCursor(0, 36);
    display.printf("Bat: %u%%", last_bat);

    display.setCursor(0, 48);
    if (repeater_active) {
        display.printf("RPT: ON  (%u)", repeater_clients);
    } else {
        display.println("RPT: OFF");
    }

    display.display();
}

static void oled_task(void *param) {
    Serial.println("[OLED] Task started");
    while (1) {
        xSemaphoreTake(oled_mutex, portMAX_DELAY);
        draw_display();
        xSemaphoreGive(oled_mutex);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void oled_task_init(void) {
    oled_mutex = xSemaphoreCreateMutex();
    Wire.begin(21, 22);
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 0);
        display.println("BLUE Mobile");
        display.display();
        Serial.println("[OLED] Initialized");
    } else {
        Serial.println("[OLED] Init failed");
    }
}

void oled_task_start(void) {
    xTaskCreatePinnedToCore(oled_task, "oled", 4096, NULL, 2, &oled_handle, 1);
}

void oled_task_update_state(mobile_state_t state, rssi_t rssi, battery_pct_t bat) {
    if (!oled_mutex) return;
    xSemaphoreTake(oled_mutex, portMAX_DELAY);
    last_state = state;
    last_rssi = rssi;
    last_bat = bat;
    xSemaphoreGive(oled_mutex);
}

void oled_task_update_repeater(bool active, uint8_t clients) {
    if (!oled_mutex) return;
    xSemaphoreTake(oled_mutex, portMAX_DELAY);
    repeater_active = active;
    repeater_clients = clients;
    xSemaphoreGive(oled_mutex);
}

#else

void oled_task_init(void) {}
void oled_task_start(void) {}
void oled_task_update_state(mobile_state_t state, rssi_t rssi, battery_pct_t bat) {
    (void)state; (void)rssi; (void)bat;
}
void oled_task_update_repeater(bool active, uint8_t clients) {
    (void)active; (void)clients;
}

#endif
