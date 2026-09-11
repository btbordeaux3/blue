/*
 * test_decision.c - Unit tests for the (glib-free) decision engine.
 * Builds/runs under CTest: same binary also exercises the hysteresis logic.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_wifi_agent/decision.h"

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static void test_hysteresis_requires_confirmation(void)
{
    esp_hyst_t h;
    bool confirm = false, gone = false;

    esp_hyst_init(&h);

    /* one blip must NOT confirm presence (flap guard) */
    esp_hyst_feed(&h, true,  2, 2, &confirm, &gone);
    CHECK(confirm == false);
    CHECK(gone   == false);

    /* a single miss must NOT declare it gone */
    esp_hyst_feed(&h, false, 2, 2, &confirm, &gone);
    CHECK(gone == false);

    /* two consecutive misses *does* */
    esp_hyst_feed(&h, false, 2, 2, &confirm, &gone);
    CHECK(gone == true);
}

static void test_decision_connect_when_confirm_present(void)
{
    esp_decision_input_t in = {
        .ext_present_signal = true,
        .ext_connected      = false,
        .ext_profile_ready  = true,
        .ext_confirm        = true,
        .ext_gone           = false,
        .previous_known     = true,
        .previous_available = true,
    };
    CHECK(esp_decision_run(&in) == ESP_ACT_CONNECT_EXT);

    /* but not without hysteresis confirmation */
    in.ext_confirm = false;
    CHECK(esp_decision_run(&in) == ESP_ACT_NONE);
}

static void test_decision_connects_on_first_run_without_profile(void)
{
    /* Fresh image: no saved NM profile for the extender yet. The daemon
     * must still switch to the ESP32 AP; connect_ext() creates the profile
     * on the fly. Previously this case never connected. */
    esp_decision_input_t in = {
        .ext_present_signal = true,
        .ext_connected      = false,
        .ext_profile_ready  = false,
        .ext_confirm        = true,
        .ext_gone           = false,
        .previous_known     = true,
        .previous_available = true,
    };
    CHECK(esp_decision_run(&in) == ESP_ACT_CONNECT_EXT);
}

static void test_decision_restore_previous_when_ext_gone(void)
{
    esp_decision_input_t in = {
        .ext_connected      = true,
        .ext_gone           = true,
        .previous_known     = true,
        .previous_available = true,
    };
    CHECK(esp_decision_run(&in) == ESP_ACT_RESTORE_PREVIOUS);

    in.previous_available = false;
    CHECK(esp_decision_run(&in) == ESP_ACT_FALLBACK);

    in.previous_known = false;
    CHECK(esp_decision_run(&in) == ESP_ACT_FALLBACK);
}

static void test_decision_stays_put_when_stable(void)
{
    esp_decision_input_t in = {
        .ext_connected      = true,
        .ext_gone           = false,
        .ext_confirm        = true,
        .previous_known     = true,
        .previous_available = true,
    };
    CHECK(esp_decision_run(&in) == ESP_ACT_NONE);
}

int main(void)
{
    test_hysteresis_requires_confirmation();
    test_decision_connect_when_confirm_present();
    test_decision_connects_on_first_run_without_profile();
    test_decision_restore_previous_when_ext_gone();
    test_decision_stays_put_when_stable();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("all decision tests passed\n");
    return 0;
}