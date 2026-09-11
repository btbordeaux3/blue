/*
 * decision.c - Pure (glib/libnm free) decision engine.
 *
 * State table:
 *
 *   connected to ext | AP clearly gone? | previous known?    -> action
 *   -----------------+------------------+--------------------+---------------
 *   yes              | yes              | yes                | RESTORE_PREVIOUS
 *   yes              | yes              | no                 | FALLBACK
 *   no               | AP clearly here  | (profile auto-made) | CONNECT_EXT
 *   otherwise                                                     NONE
 */

#include "esp_wifi_agent/decision.h"

void esp_hyst_init(esp_hyst_t *h)
{
    h->present_streak = 0;
    h->absent_streak  = 0;
}

void esp_hyst_feed(esp_hyst_t *h,
                   bool        signal,
                   int         req_seen,
                   int         req_missed,
                   bool       *confirm,
                   bool       *gone)
{
    if (req_seen < 1)   req_seen   = 1;
    if (req_missed < 1) req_missed = 1;

    if (signal) {
        h->present_streak++;
        h->absent_streak = 0;
        *confirm = (h->present_streak >= req_seen);
        *gone    = false;
    } else {
        h->absent_streak++;
        h->present_streak = 0;
        *gone    = (h->absent_streak >= req_missed);
        *confirm = false;
    }
}

esp_agent_action_t esp_decision_run(const esp_decision_input_t *in)
{
    if (in->ext_connected) {
        if (in->ext_gone) {
            /* Extender vanished (three sources of hint: scan, USB, link loss):
             * prefer the previous network if we know it and can reach it,
             * otherwise let NetworkManager's autoconnect order decide. */
            return (in->previous_known && in->previous_available)
                       ? ESP_ACT_RESTORE_PREVIOUS
                       : ESP_ACT_FALLBACK;
        }
        return ESP_ACT_NONE;
    }

    /* Not on the extender: if it is clearly present (wifi scan and/or USB
     * telemetry, passed through hysteresis), switch to it. No saved NM
     * profile is required - connect_ext() creates one on the fly, which is
     * exactly what must happen on first boot of a fresh image. */
    if (in->ext_confirm)
        return ESP_ACT_CONNECT_EXT;

    return ESP_ACT_NONE;
}