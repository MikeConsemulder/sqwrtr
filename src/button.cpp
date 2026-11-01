#include "button.h"
#include "config.h"
#include "zones.h"
#include "net.h"
#include "log.h"

namespace button {

static bool stable = false;    // debounced state (true = pressed)
static bool lastRaw = false;
static uint32_t rawChangedAt = 0;
static uint32_t pressedAt = 0;
static bool estopDone = false;
static bool apDone = false;

void begin() {
    pinMode(PIN_BUTTON, INPUT_PULLUP);
}

void loop() {
    const uint32_t now = millis();
    const bool raw = digitalRead(PIN_BUTTON) == LOW;  // active LOW

    if (raw != lastRaw) {
        lastRaw = raw;
        rawChangedAt = now;
    }
    if (raw != stable && now - rawChangedAt >= BUTTON_DEBOUNCE_MS) {
        stable = raw;
        if (stable) {
            pressedAt = now;
            estopDone = apDone = false;
        }
    }
    if (!stable) return;

    const uint32_t held = now - pressedAt;
    if (!estopDone && held >= BUTTON_ESTOP_MS) {
        estopDone = true;
        LOG("Button: emergency stop");
        zones::emergencyStop(zones::Source::Button);
    }
    if (!apDone && held >= BUTTON_AP_MS) {
        apDone = true;
        LOG("Button: AP mode");
        net::startApManual();
    }
}

}  // namespace button
