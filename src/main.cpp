// sqwrtr – fase 4: web-UI + opgeslagen instellingen
// Veilige pin-init, WiFi + fallback-AP, mDNS, ArduinoOTA, zone-manager met
// veiligheidsregels, FLASH-knop, status-LED, serieel testconsole en webserver.

#include <Arduino.h>
#include "config.h"
#include "log.h"
#include "events.h"
#include "relays.h"
#include "zones.h"
#include "storage.h"
#include "net.h"
#include "web.h"
#include "button.h"
#include "console.h"
#include "status_led.h"

// Zo vroeg mogelijk in de boot (vóór setup) de relais veilig uit zetten.
extern "C" void preinit() {
    relays::safeInit();
}

static void emitBootEvent() {
    uint8_t prev = relays::previousMask();
    char list[24] = "";
    size_t n = 0;
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        if (prev & (1 << i)) n += snprintf(list + n, sizeof(list) - n, "%s%u", n ? "," : "", i + 1);
    }
    events::emit("boot", "{\"versie\":\"%s\",\"reset_reden\":\"%s\",\"kleppen_open_bij_reboot\":[%s]}",
                 FW_VERSION, ESP.getResetReason().c_str(), list);
    if (prev) LOG("LET OP: kleppen %s stonden open bij reboot – nu dicht", list);
}

void setup() {
    relays::safeInit();  // nogmaals, voor de zekerheid
    relays::begin();
    status_led::begin();
    button::begin();

#if CONSOLE_ENABLED
    Serial.begin(115200);
#else
    Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);  // RX vrij voor flowmeter
#endif
    Serial.println();
    LOG("sqwrtr v%s – chip-id %06x, vrij heap %u", FW_VERSION, ESP.getChipId(), ESP.getFreeHeap());
    emitBootEvent();

    // Software-watchdog expliciet aan; hardware-watchdog staat altijd aan.
    ESP.wdtEnable(WDTO_8S);

    zones::begin();
    storage::begin();  // overschrijft standaardwaarden met opgeslagen config
    net::begin({
        []() { return zones::activeCount() == 0 && !web::updateInProgress(); },  // geen OTA met open kleppen
        []() { zones::closeAll(zones::Source::System, "ota"); relays::allOff(); },
    });
    web::begin();
}

void loop() {
    net::loop();
    web::loop();
    zones::loop();
    storage::loop();
    button::loop();
    console::loop();

    using status_led::Pattern;
    if (zones::anyOpen())           status_led::set(Pattern::On);
    else if (net::apActive())       status_led::set(Pattern::FastBlink);
    else if (!net::wifiConnected()) status_led::set(Pattern::SlowBlink);
    else                            status_led::set(Pattern::Off);
    status_led::loop();
}
