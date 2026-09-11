/*
 * Safe parking firmware for the mobile ESP32.
 *
 * This target intentionally does not initialize Wi-Fi, ESPNOW, USB command
 * handling, the repeater, sensors, or any other BLUE subsystem. It is used to
 * keep the mobile board electrically present without allowing it to change
 * the base/host network while the real mobile firmware is being prepared.
 *
 * The production mobile firmware remains in src/mobile/main.cpp and is built
 * by the separate `mobile` PlatformIO environment.
 */

#include <Arduino.h>

void setup() {
    // Serial is intentionally not started: this firmware must remain inert.
}

void loop() {
    // Do nothing. In particular, do not call any Wi-Fi or project APIs.
    delay(1000);
}
