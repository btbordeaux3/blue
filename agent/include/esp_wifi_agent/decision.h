/*
 * decision.h - Pure decision engine for the ESP32 <-> WiFi handoff daemon.
 *
 * This module is deliberately free of glib/libnm dependencies so it can be
 * unit-tested in isolation (see tests/test_decision.c, wired into CTest).
 */

#ifndef ESP_WIFI_AGENT_DECISION_H
#define ESP_WIFI_AGENT_DECISION_H

#include <stdbool.h>

typedef enum {
    ESP_ACT_NONE = 0,            /* stay put */
    ESP_ACT_CONNECT_EXT,         /* activate the ESP32 extender AP */
    ESP_ACT_RESTORE_PREVIOUS,    /* leave ext, go back to the previous network */
    ESP_ACT_FALLBACK,            /* leave ext, let NetworkManager pick best available */
} esp_agent_action_t;

/*
 * Hysteresis tracker. Prevents flapping when the extender AP blips:
 * the AP must be seen for `req_seen` consecutive ticks before it counts
 * as "present", and missed for `req_missed` before it counts as "gone".
 */
typedef struct {
    int present_streak;
    int absent_streak;
} esp_hyst_t;

typedef struct {
    bool ext_present_signal;     /* raw observation this tick (wifi scan and/or USB telemetry) */
    bool ext_connected;          /* currently associated with the extender AP */
    bool ext_profile_ready;      /* an activatable NM connection exists for the extender */
    bool previous_known;         /* we have a recorded previous connection */
    bool previous_available;     /* previous connection is still visible in scans */
    bool ext_confirm;            /* hysteresis: AP is clearly present */
    bool ext_gone;               /* hysteresis: AP is clearly gone */
} esp_decision_input_t;

void esp_hyst_init(esp_hyst_t *h);

/* Feed one observation; updates *confirm / *gone. Pure, no allocation. */
void esp_hyst_feed(esp_hyst_t       *h,
                   bool              signal,
                   int               req_seen,
                   int               req_missed,
                   bool             *confirm,
                   bool             *gone);

esp_agent_action_t esp_decision_run(const esp_decision_input_t *in);

#endif /* ESP_WIFI_AGENT_DECISION_H */