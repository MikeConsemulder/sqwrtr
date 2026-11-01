#pragma once
#include <Arduino.h>

namespace status_led {

enum class Pattern : uint8_t {
    Off,        // all ok / idle
    SlowBlink,  // no WiFi
    FastBlink,  // AP mode
    On,         // valve open
};

void begin();
void set(Pattern p);
void loop();

}  // namespace status_led
