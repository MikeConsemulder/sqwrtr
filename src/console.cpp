#include "console.h"
#include <Arduino.h>
#include "config.h"
#include "zones.h"
#include "net.h"
#include "relays.h"

#if CONSOLE_ENABLED

namespace console {

static char line[48];
static uint8_t len = 0;

static const char* stateName(zones::State s) {
    switch (s) {
        case zones::State::Open: return "OPEN";
        case zones::State::Pending: return "wait";
        default: return "closed";
    }
}

static void printStatus() {
    Serial.printf_P(PSTR("Uptime %lu s, heap %u, WiFi %s, AP %s, relays 0x%02x\n"),
                    millis() / 1000, ESP.getFreeHeap(), net::wifiConnected() ? "yes" : "no",
                    net::apActive() ? "on" : "off", relays::mask());
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        Serial.printf_P(PSTR("  %u %-12s %-6s left %lu s\n"), i + 1, zones::config(i).name,
                        stateName(zones::state(i)), (unsigned long)zones::remainingS(i));
    }
}

static void printHelp() {
    Serial.print(F("Commands:\n"
                   "  status            zones and system\n"
                   "  open <n> [sec]    open zone n (without sec: indefinite, max runtime)\n"
                   "  close <n>         close zone n\n"
                   "  stop              emergency stop\n"
                   "  ap                turn on fallback AP\n"
                   "  reboot            restart\n"));
}

static void handle(char* cmd) {
    char* verb = strtok(cmd, " ");
    if (!verb) return;
    char* a1 = strtok(nullptr, " ");
    char* a2 = strtok(nullptr, " ");

    if (!strcmp(verb, "status")) {
        printStatus();
    } else if (!strcmp(verb, "open") && a1) {
        uint8_t z = atoi(a1) - 1;
        uint32_t secs = a2 ? strtoul(a2, nullptr, 10) : 0;
        zones::Result r = zones::open(z, secs, zones::Source::Console);
        Serial.printf_P(PSTR("open: %s\n"), zones::resultName(r));
    } else if (!strcmp(verb, "close") && a1) {
        bool ok = zones::close(atoi(a1) - 1, zones::Source::Console);
        Serial.println(ok ? F("close: ok") : F("close: zone was already closed"));
    } else if (!strcmp(verb, "stop")) {
        zones::emergencyStop(zones::Source::Console);
    } else if (!strcmp(verb, "ap")) {
        net::startApManual();
    } else if (!strcmp(verb, "reboot")) {
        Serial.println(F("Restarting..."));
        Serial.flush();
        ESP.restart();
    } else {
        printHelp();
    }
}

void loop() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (len) {
                line[len] = '\0';
                handle(line);
                len = 0;
            }
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }
}

}  // namespace console

#else

namespace console {
void loop() {}
}  // namespace console

#endif
