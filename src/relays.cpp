#include "relays.h"
#include "config.h"

namespace relays {

// Survives a (watchdog) reset, not a power loss.
struct RtcState {
    uint32_t magic;
    uint32_t mask;
};
static const uint32_t RTC_MAGIC = 0x53515731;  // "SQW1"

static uint8_t current = 0;
static uint8_t previous = 0;

static void setupOff(uint8_t pin) {
    digitalWrite(pin, HIGH);  // active LOW → HIGH = off
    pinMode(pin, OUTPUT);
}

static void saveRtc() {
    RtcState s = {RTC_MAGIC, current};
    ESP.rtcUserMemoryWrite(RTC_OFFSET_RELAYS, reinterpret_cast<uint32_t*>(&s), sizeof(s));
}

void safeInit() {
    for (uint8_t i = 0; i < ZONE_COUNT; i++) setupOff(ZONE_PINS[i]);
    setupOff(PIN_MASTER_VALVE);
}

void begin() {
    RtcState s;
    if (ESP.rtcUserMemoryRead(RTC_OFFSET_RELAYS, reinterpret_cast<uint32_t*>(&s), sizeof(s)) &&
        s.magic == RTC_MAGIC) {
        previous = s.mask;
    }
    current = 0;
    saveRtc();
}

uint8_t previousMask() { return previous; }

void setZone(uint8_t zone, bool on) {
    if (zone >= ZONE_COUNT) return;
    digitalWrite(ZONE_PINS[zone], on ? LOW : HIGH);
    if (on) current |= (1 << zone);
    else current &= ~(1 << zone);
    saveRtc();
}

uint8_t mask() { return current; }

void allOff() {
    for (uint8_t i = 0; i < ZONE_COUNT; i++) digitalWrite(ZONE_PINS[i], HIGH);
    digitalWrite(PIN_MASTER_VALVE, HIGH);
    current = 0;
    saveRtc();
}

}  // namespace relays
