// sqwrtr – fase 1: fundament
// Veilige pin-init, WiFi + fallback-AP, mDNS, ArduinoOTA, serieel loggen, status-LED.

#include <Arduino.h>
#include "config.h"
#include "log.h"
#include "relays.h"
#include "net.h"
#include "status_led.h"

// Zo vroeg mogelijk in de boot (vóór setup) de relais veilig uit zetten.
extern "C" void preinit() {
    relays::safeInit();
}

void setup() {
    relays::safeInit();  // nogmaals, voor de zekerheid
    status_led::begin();

    Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);  // RX vrij voor flowmeter
    Serial.println();
    LOG("sqwrtr v%s – boot, reset-reden: %s", FW_VERSION, ESP.getResetReason().c_str());
    LOG("Chip-id %06x, vrij heap %u", ESP.getChipId(), ESP.getFreeHeap());
    // TODO fase 2: event boot (+ "kleppen waren open bij reboot" via RTC-geheugen)

    // Software-watchdog expliciet aan; hardware-watchdog staat altijd aan.
    ESP.wdtEnable(WDTO_8S);

    net::begin([]() {
        relays::allOff();  // nooit updaten met open kleppen
        // TODO fase 2: OTA weigeren als er een klep open is + event update_started
    });
}

void loop() {
    net::loop();

    using status_led::Pattern;
    if (net::apActive())            status_led::set(Pattern::FastBlink);
    else if (!net::wifiConnected()) status_led::set(Pattern::SlowBlink);
    else                            status_led::set(Pattern::Off);
    // TODO fase 2: Pattern::On als er een klep open is
    status_led::loop();
}
