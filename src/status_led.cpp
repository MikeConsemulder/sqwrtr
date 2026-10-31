#include "status_led.h"
#include "config.h"

namespace status_led {

static Pattern current = Pattern::Off;
static bool lit = false;
static uint32_t lastToggle = 0;

static void write(bool on) {
    lit = on;
    digitalWrite(PIN_LED, on ? LOW : HIGH);  // active LOW
}

void begin() {
    digitalWrite(PIN_LED, HIGH);
    pinMode(PIN_LED, OUTPUT);
}

void set(Pattern p) {
    if (p == current) return;
    current = p;
    lastToggle = millis();
    write(p == Pattern::On);
}

void loop() {
    uint32_t period;
    switch (current) {
        case Pattern::SlowBlink: period = 1000; break;
        case Pattern::FastBlink: period = 150; break;
        default: return;
    }
    uint32_t now = millis();
    if (now - lastToggle >= period) {
        lastToggle = now;
        write(!lit);
    }
}

}  // namespace status_led
