#pragma once
#include <Arduino.h>
#include <functional>

namespace net {

struct OtaHooks {
    std::function<bool()> allowed;  // false → OTA invitations are ignored
    std::function<void()> onStart;  // right before an OTA update starts
};

// WiFi (STA), fallback AP, mDNS and ArduinoOTA.
void begin(const OtaHooks& ota);
void loop();

// Turn the AP on manually (FLASH button); stays on for at least WIFI_AP_MANUAL_MS.
void startApManual();

bool wifiConnected();
bool apActive();

}  // namespace net
