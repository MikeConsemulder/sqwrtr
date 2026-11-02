// sqwrtr – phase 6: MQTT + Home Assistant discovery
// Safe pin init, WiFi + fallback AP, mDNS, ArduinoOTA, zone manager with
// safety rules, FLASH button, status LED, serial test console and web server.

#include <Arduino.h>
#include "config.h"
#include "log.h"
#include "events.h"
#include "relays.h"
#include "zones.h"
#include "storage.h"
#include "programs.h"
#include "history.h"
#include "mqtt.h"
#include "ntp.h"
#include "net.h"
#include "web.h"
#include "button.h"
#include "console.h"
#include "status_led.h"

// Switch the relays safely off as early in the boot as possible (before setup).
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
    events::emit("boot", "{\"version\":\"%s\",\"reset_reason\":\"%s\",\"valves_open_at_reboot\":[%s]}",
                 FW_VERSION, ESP.getResetReason().c_str(), list);
    if (prev) LOG("WARNING: valves %s were open at reboot – now closed", list);
}

void setup() {
    relays::safeInit();  // once more, to be safe
    relays::begin();
    status_led::begin();
    button::begin();

#if CONSOLE_ENABLED
    Serial.begin(115200);
#else
    Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);  // RX free for the flow meter
#endif
    Serial.println();
    LOG("sqwrtr v%s – chip id %06x, free heap %u", FW_VERSION, ESP.getChipId(), ESP.getFreeHeap());
    emitBootEvent();

    // Software watchdog explicitly on; the hardware watchdog is always on.
    ESP.wdtEnable(WDTO_8S);

    zones::begin();
    programs::begin();
    mqtt::begin();     // MQTT defaults from secrets.h, before storage loads the saved config
    storage::begin();  // overrides defaults with the saved config (including time zone)
    history::begin();
    history::add(history::Action::Boot, 0, 0, relays::previousMask(), (uint8_t)zones::Source::System, "");
    ntp::begin();
    net::begin({
        []() { return zones::activeCount() == 0 && !web::updateInProgress(); },  // no OTA with open valves
        []() {
            programs::abortAll("aborted");
            zones::closeAll(zones::Source::System, "ota");
            relays::allOff();
            storage::flush();
            history::flush();
        },
        []() { return zones::activeCount() == 0 && !programs::busy() && !web::updateInProgress(); },
        []() {
            storage::flush();
            history::flush();
        },
    });
    web::begin();
}

void loop() {
    net::loop();
    web::loop();
    mqtt::loop();
    zones::loop();
    programs::loop();
    storage::loop();
    history::loop();
    button::loop();
    console::loop();

    using status_led::Pattern;
    if (zones::anyOpen())           status_led::set(Pattern::On);
    else if (net::apActive())       status_led::set(Pattern::FastBlink);
    else if (!net::wifiConnected()) status_led::set(Pattern::SlowBlink);
    else                            status_led::set(Pattern::Off);
    status_led::loop();
}
