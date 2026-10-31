#pragma once
#include <Arduino.h>
#include <functional>

namespace net {

struct OtaHooks {
    std::function<bool()> allowed;  // false → OTA-uitnodigingen worden genegeerd
    std::function<void()> onStart;  // vlak voordat een OTA-update begint
};

// WiFi (STA), fallback-AP, mDNS en ArduinoOTA.
void begin(const OtaHooks& ota);
void loop();

// AP handmatig aanzetten (FLASH-knop); blijft minimaal WIFI_AP_MANUAL_MS aan.
void startApManual();

bool wifiConnected();
bool apActive();

}  // namespace net
