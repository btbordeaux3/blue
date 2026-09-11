/*
 * power_manager.cpp - Mobile unit state machine and power management
 *
 * Controls the mobile unit's lifecycle using a finite state machine:
 *
 *   SLEEP → SCAN → EVALUATE → REPORT → SLEEP (normal cycle)
 *                                  ↓
 *                              REPEATER (weak signal detected)
 *                                  ↓
 *                              SLEEP (deactivated)
 *
 * Power management:
 *   - ESP32 deep sleep between scans (timer + GPIO wake sources)
 *   - WiFi disconnects during sleep; reconnects after wake
 *   - Watchdog feed (esp_task_wdt_reset) during WiFi reconnect
 *   - metrics_track_* calls for WCET and timing analysis
 *
 * Interview talking points:
 *   - State machine pattern prevents ad-hoc if/else spaghetti
 *   - Event group (xEventGroupWaitBits) for efficient state transitions
 *   - Light sleep vs deep sleep: light sleep keeps SRAM, wakes in ~5ms
 *   - WiFi channel persistence: ESP32 remembers channel across sleep
 *   - Repeater mode uses WiFi.mode(WIFI_AP_STA) dual-role
 *   - LED logic: active-low (LOW=on) matches most ESP32 dev boards
 */

#include "power_manager.h"
#include "espnow_comm.h"
#include "mobile_scan_task.h"
#include "report_task.h"
#include "repeater_task.h"
#include "metrics.h"
#include "common.h"
#include "usb_link.h"
#include <esp_task_wdt.h>

static TaskHandle_t pm_handle = NULL;
static mobile_state_t current_state = STATE_SLEEP;
static mobile_state_t target_state = STATE_SLEEP;
static bool activate_requested = false;
static bool deactivate_requested = false;
static deactivation_reason_t deactivate_reason = DEACT_BASE_CMD;
static uint32_t state_entered_at = 0;
static uint32_t repeater_started_at = 0;
static uint32_t weak_signal_since = 0;
static uint32_t recovered_since = 0;
static uint32_t last_scan_at = 0;
static bool espnow_cmd_received = false;
static uint32_t last_state_report_ms = 0;
static uint8_t consecutive_dead_scans = 0;
static command_msg_t last_cmd;
static activation_source_t current_activation_source = SRC_NONE;
static uint8_t activation_confidence = 100;

static EventGroupHandle_t state_events;
#define EVT_SCAN_DONE      (1 << 0)
#define EVT_CMD_ACTIVATE   (1 << 1)
#define EVT_CMD_DEACTIVATE (1 << 2)
#define EVT_WEAK_SIGNAL    (1 << 3)
#define EVT_GOOD_SIGNAL    (1 << 4)
#define EVT_BUTTON_PRESS   (1 << 5)
#define EVT_SLEEP_NOW      (1 << 6)
#define EVT_BUFFER_FLUSH   (1 << 7)

static void on_command_received(const command_msg_t *cmd) {
    memcpy(&last_cmd, cmd, sizeof(command_msg_t));
    espnow_cmd_received = true;

    if (cmd->command == CMD_ACTIVATE_REPEATER) {
        current_activation_source = cmd->source ? cmd->source : SRC_BASE_MANUAL;
        activation_confidence = cmd->confidence_pct ? cmd->confidence_pct : 100;
        xEventGroupSetBits(state_events, EVT_CMD_ACTIVATE);
        Serial.printf("[PM] Base command: ACTIVATE (src=%d conf=%u%%)\n",
                      current_activation_source, activation_confidence);
    } else if (cmd->command == CMD_DEACTIVATE || cmd->command == CMD_SLEEP) {
        xEventGroupSetBits(state_events, EVT_CMD_DEACTIVATE);
        Serial.println("[PM] Base command: DEACTIVATE");
    } else if (cmd->command == CMD_SEND_BUFFERED) {
        xEventGroupSetBits(state_events, EVT_BUFFER_FLUSH);
    }
}

static void enter_sleep_state(void) {
    current_state = STATE_SLEEP;
    state_entered_at = millis();
    Serial.println("[PM] Entering SLEEP");
    metrics_log_event(EVT_STATE_TRANSITION, STATE_SLEEP, 0);

    repeater_task_deactivate();
    mobile_scan_task_stop();

    vTaskDelay(pdMS_TO_TICKS(100));

    esp_sleep_enable_timer_wakeup(SCAN_INTERVAL_MS * 1000);
    esp_sleep_enable_gpio_wakeup();

    Serial.flush();
    metrics_on_sleep_enter();
    esp_light_sleep_start();
    metrics_on_sleep_wake();

    Serial.println("[PM] Woke from sleep");

    uint32_t connect_start = millis();
    int wait = 0;
    while (WiFi.status() != WL_CONNECTED && wait < 50) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
        wait++;
    }
    uint32_t connect_time = millis() - connect_start;
    if (WiFi.status() == WL_CONNECTED) {
        metrics_on_wifi_connect();
        metrics_record_wifi_connect_time(connect_time);
    }

    current_state = STATE_SCAN;
    state_entered_at = millis();
    last_scan_at = millis();
    mobile_scan_task_start();
}

static void enter_scan_state(void) {
    current_state = STATE_SCAN;
    state_entered_at = millis();
    Serial.println("[PM] Entering SCAN");

    mobile_scan_task_start();
    last_scan_at = millis();
}

static void enter_evaluate_state(rf_fingerprint_t *fp) {
    current_state = STATE_EVALUATE;
    state_entered_at = millis();

    rssi_t avg = rf_average_top_n(fp, 3);
    bool weak = rf_is_weak_signal(fp);

    metrics_record_rssi(avg);
    metrics_record_scan_ap_count(fp->count);

    Serial.printf("[PM] EVALUATE: avg RSSI=%d, weak=%s\n", avg, weak ? "YES" : "no");

    if (weak) {
        if (weak_signal_since == 0) weak_signal_since = millis();
        metrics_set_weak_signal_time(weak_signal_since);
        xEventGroupSetBits(state_events, EVT_WEAK_SIGNAL);
    } else {
        weak_signal_since = 0;
        xEventGroupSetBits(state_events, EVT_GOOD_SIGNAL);
    }
}

static void enter_report_state(rf_fingerprint_t *fp) {
    current_state = STATE_REPORT;
    state_entered_at = millis();
    Serial.println("[PM] Entering REPORT");

    report_task_send_fingerprint(fp);
    vTaskDelay(pdMS_TO_TICKS(200));
}

static void enter_repeater_state(void) {
    if (current_state == STATE_REPEATER) return;

    current_state = STATE_REPEATER;
    state_entered_at = millis();
    repeater_started_at = millis();
    recovered_since = 0;

    Serial.println("[PM] Entering REPEATER");
    metrics_log_event(EVT_STATE_TRANSITION, STATE_REPEATER, 0);
    metrics_on_repeater_activate();
    metrics_record_activation_latency(millis());

    /* Stop the scan task BEFORE bridging: every scan sweeps channels and
     * temporarily drops the STA link, which would kill the extender's
     * internet and make clients flap. */
    mobile_scan_task_stop();

    repeater_task_activate();
}

static void exit_repeater_state(void) {
    current_state = STATE_SLEEP;
    state_entered_at = millis();
    repeater_started_at = 0;
    weak_signal_since = 0;
    recovered_since = 0;

    Serial.println("[PM] Exiting REPEATER -> SLEEP");
    metrics_log_event(EVT_STATE_TRANSITION, STATE_SLEEP, STATE_REPEATER);
    metrics_on_repeater_deactivate();
    repeater_task_deactivate();
    usb_link_report("AP_DOWN", 0);
}

static void handle_repeater_timeout(void) {
    if (current_state != STATE_REPEATER) return;
    if (repeater_started_at == 0) return;

    if (millis() - repeater_started_at > BRIDGE_SAFETY_TIMEOUT_MS) {
        Serial.println("[PM] Repeater safety timeout - deactivating");
        exit_repeater_state();
    }
}

static void handle_connection_loss(void) {
    connection_status_t status = espnow_get_connection_status();
    if (status == LOST && current_state != STATE_RECONNECT) {
        Serial.println("[PM] Connection to base lost - entering local mode");
        current_state = STATE_LOST;
    }
}

static void handle_reconnect(void) {
    if (current_state == STATE_LOST || current_state == STATE_RECONNECT) {
        if (espnow_get_connection_status() == CONNECTED) {
            Serial.println("[PM] Reconnected to base");
            reconnect_msg_t re;
            re.msg_type = MSG_RECONNECT;
            re.reason = LOSS_TIMEOUT;
            re.last_seen_timestamp = millis();
            re.buffered_count = report_task_get_buffered_count();
            espnow_send_reconnect(&re);
            current_state = STATE_SLEEP;
        }
    }
}

static void power_manager_task(void *param) {
    Serial.println("[PM] Task started");

    espnow_set_cmd_callback(on_command_received);

    esp_task_wdt_add(NULL);

    while (1) {
        esp_task_wdt_reset();

        EventBits_t bits = xEventGroupWaitBits(state_events,
            EVT_CMD_ACTIVATE | EVT_CMD_DEACTIVATE | EVT_WEAK_SIGNAL |
            EVT_GOOD_SIGNAL | EVT_SLEEP_NOW | EVT_BUFFER_FLUSH,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(100));

        if (bits & EVT_CMD_ACTIVATE) {
            activate_requested = true;
        }
        if (bits & EVT_CMD_DEACTIVATE) {
            deactivate_requested = true;
        }
        if (bits & EVT_BUFFER_FLUSH) {
            report_task_flush_buffer();
        }

        handle_reconnect();

        switch (current_state) {
            case STATE_SLEEP:
                handle_connection_loss();
                enter_sleep_state();
                break;

            case STATE_SCAN:
                if (millis() - last_scan_at >= 2000) {
                    rf_fingerprint_t fp;
                    memset(&fp, 0, sizeof(fp));
                    if (mobile_scan_get_fingerprint(&fp)) {
                        enter_evaluate_state(&fp);
                    } else {
                        enter_report_state(&fp);
                    }
                }
                break;

            case STATE_EVALUATE: {
                rf_fingerprint_t fp;
                memset(&fp, 0, sizeof(fp));
                if (mobile_scan_get_fingerprint(&fp)) {
                    rssi_t avg = rf_average_valid_top_n(&fp, 3);

                    /* Ground truth for a dead zone: the mobile's OWN uplink to
                     * the router. A scan that returns only -128 placeholders
                     * (radio glitch / momentary disconnect) is NOT a dead zone
                     * by itself - check the live link before doing anything. */
                    rssi_t link_rssi = (WiFi.status() == WL_CONNECTED)
                        ? (rssi_t)WiFi.RSSI() : (rssi_t)-128;

                    bool scan_weak = avg >= -127 && avg < WEAK_RSSI_THRESHOLD;
                    bool link_weak = link_rssi < WEAK_RSSI_THRESHOLD;

                    if (avg <= -128 && link_rssi >= WEAK_RSSI_THRESHOLD) {
                        /* Empty scan but the router link is fine: keep cycling,
                         * do NOT flip the bridge. This squelches the original
                         * flap (empty scan -> "weak" -> activate -> recover). */
                        consecutive_dead_scans = 0;
                        Serial.printf("[PM] EVALUATE: no APs, uplink %d dBm - OK, holding\n",
                                      link_rssi);
                        enter_report_state(&fp);
                        break;
                    }

                    if (avg <= -128 && link_rssi <= -128) {
                        /* Neither scan nor uplink data available. Only treat as a
                         * dead zone after several consecutive empty cycles, so a
                         * transient radio glitch cannot flip the bridge on. */
                        consecutive_dead_scans++;
                        if (consecutive_dead_scans < DEAD_ZONE_CONFIRM_SCANS) {
                            Serial.printf("[PM] EVALUATE: no data (%u/%u) - holding\n",
                                          consecutive_dead_scans, DEAD_ZONE_CONFIRM_SCANS);
                            enter_report_state(&fp);
                            break;
                        }
                        Serial.println("[PM] EVALUATE: sustained dead zone - activating");
                    }

                    if (activate_requested || scan_weak || link_weak ||
                        consecutive_dead_scans >= DEAD_ZONE_CONFIRM_SCANS) {
                        consecutive_dead_scans = 0;
                        if (!activate_requested) {
                            /* The mobile detected the weak link itself and is
                             * turning the repeater on reactively; this is the
                             * MOBILE_LOW trigger for the dashboard. */
                            current_activation_source = SRC_MOBILE_RSSI;
                            rssi_t worst = (link_weak && link_rssi > -127)
                                ? link_rssi : avg;
                            activation_confidence = (uint8_t)((worst + 80) > 60 ? 60
                                                              : (worst + 80));
                            if (activation_confidence > 95) activation_confidence = 95;
                            if (activation_confidence < 30) activation_confidence = 30;
                        }
                        activate_requested = false;
                        usb_link_report("AP_UP", avg);
                        enter_repeater_state();
                    } else {
                        consecutive_dead_scans = 0;
                        enter_report_state(&fp);
                    }
                } else {
                    enter_report_state(&fp);
                }
                break;
            }

            case STATE_REPORT:
                vTaskDelay(pdMS_TO_TICKS(300));
                if (deactivate_requested) {
                    deactivate_requested = false;
                    exit_repeater_state();
                } else {
                    current_state = STATE_SLEEP;
                }
                break;

            case STATE_REPEATER:
                handle_repeater_timeout();
                if (deactivate_requested) {
                    deactivate_requested = false;
                    exit_repeater_state();
                    break;
                }

                /* Keep the base informed we are bridging: while REPEATER the
                 * scan task is suspended so no fingerprints flow, and the
                 * base would show the extender as OFF. Re-send the last
                 * fingerprint at a low cadence so the base's state sync sees
                 * local_state == REPEATER and pushes decision active=true. */
                if (millis() - last_state_report_ms >= STATE_REPORT_INTERVAL_MS) {
                    last_state_report_ms = millis();
                    rf_fingerprint_t fp;
                    if (mobile_scan_get_fingerprint(&fp)) {
                        report_task_send_fingerprint(&fp);
                    }
                }

                /* Turn OFF only when the router link is MEASURABLY strong for a
                 * sustained window - never on a fixed timer. weak_signal_since
                 * only says WHEN weakness started; it must not drive recovery. */
                if (WiFi.status() == WL_CONNECTED &&
                    (millis() - repeater_started_at) > REPEATER_MIN_ACTIVE_MS) {
                    if (WiFi.RSSI() >= STRONG_RSSI_THRESHOLD) {
                        if (recovered_since == 0) {
                            recovered_since = millis();
                            Serial.printf("[PM] Link recovered (%d dBm) - monitoring\n",
                                          WiFi.RSSI());
                        } else if ((millis() - recovered_since) > REPEATER_RECOVERY_MS) {
                            Serial.println("[PM] Link strong for 30s - deactivating");
                            exit_repeater_state();
                        }
                    } else {
                        recovered_since = 0;
                    }
                }
                break;

            case STATE_LOST:
            case STATE_RECONNECT:
                handle_reconnect();
                if (espnow_get_connection_status() == CONNECTED) {
                    current_state = STATE_SLEEP;
                } else if (millis() - last_scan_at >= 2000) {
                    rf_fingerprint_t fp;
                    if (mobile_scan_get_fingerprint(&fp)) {
                        enter_report_state(&fp);
                        current_state = STATE_LOST;
                    }
                    last_scan_at = millis();
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            default:
                current_state = STATE_SLEEP;
                break;
        }
    }
}

void power_manager_init(void) {
    state_events = xEventGroupCreate();
}

void power_manager_start(void) {
    xTaskCreatePinnedToCore(power_manager_task, "pm", 8192, NULL, 6, &pm_handle, 0);
    metrics_set_pm_handle(pm_handle);
}

void power_manager_set_state(mobile_state_t new_state) {
    target_state = new_state;
}

mobile_state_t power_manager_get_state(void) {
    return current_state;
}

void power_manager_request_activate(void) {
    activate_requested = true;
    xEventGroupSetBits(state_events, EVT_CMD_ACTIVATE);
}

void power_manager_request_deactivate(deactivation_reason_t reason) {
    deactivate_reason = reason;
    deactivate_requested = true;
    xEventGroupSetBits(state_events, EVT_CMD_DEACTIVATE);
}

void power_manager_enter_sleep(uint32_t sleep_duration_ms) {
    xEventGroupSetBits(state_events, EVT_SLEEP_NOW);
}

void power_manager_print_status(void) {
    const char *state_names[] = {"SLEEP", "SCAN", "EVAL", "REPORT", "REPEATER", "RECONNECT", "LOST"};
    Serial.printf("[PM] State: %s, uptime: %lu ms\n",
                  state_names[current_state],
                  millis() - state_entered_at);
}

TaskHandle_t power_manager_get_task_handle(void) {
    return pm_handle;
}

activation_source_t power_manager_get_activation_source(void) {
    return current_activation_source;
}

uint8_t power_manager_get_activation_confidence(void) {
    return activation_confidence;
}
