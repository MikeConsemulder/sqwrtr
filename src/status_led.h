#pragma once
#include <Arduino.h>

namespace status_led {

enum class Pattern : uint8_t {
    Off,        // alles ok / rust
    SlowBlink,  // geen WiFi
    FastBlink,  // AP-modus
    On,         // klep open
};

void begin();
void set(Pattern p);
void loop();

}  // namespace status_led
