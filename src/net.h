#pragma once
#include <Arduino.h>
#include <functional>

namespace net {

// WiFi (STA), fallback-AP, mDNS en ArduinoOTA.
// onOtaStart wordt aangeroepen vlak voordat een OTA-update begint.
void begin(std::function<void()> onOtaStart);
void loop();

bool wifiConnected();
bool apActive();

}  // namespace net
