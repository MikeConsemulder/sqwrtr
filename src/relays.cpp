#include "relays.h"
#include "config.h"

namespace relays {

static void setupOff(uint8_t pin) {
    digitalWrite(pin, HIGH);  // active LOW → HIGH = uit
    pinMode(pin, OUTPUT);
}

void safeInit() {
    for (uint8_t i = 0; i < ZONE_COUNT; i++) setupOff(ZONE_PINS[i]);
    setupOff(PIN_MASTER_VALVE);
}

void allOff() {
    for (uint8_t i = 0; i < ZONE_COUNT; i++) digitalWrite(ZONE_PINS[i], HIGH);
    digitalWrite(PIN_MASTER_VALVE, HIGH);
}

}  // namespace relays
