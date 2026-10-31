#include "zones.h"
#include "config.h"
#include "relays.h"
#include "events.h"
#include "log.h"

namespace zones {

struct Run {
    State state;
    Source src;
    bool capped;         // looptijd begrensd door maxRuntimeS (onbepaald of te lang gevraagd)
    uint32_t seq;        // volgorde van aanvragen, voor de wachtende zones
    uint32_t openedAt;   // millis() bij openen relais
    uint32_t runMs;      // totale looptijd vanaf openedAt
};

static Config cfg[ZONE_COUNT];
static Run runs[ZONE_COUNT];
static Settings globalSettings = {DEFAULT_MAX_CONCURRENT, DEFAULT_STAGGER_MS};
static uint32_t nextSeq = 0;
static uint32_t lastSwitchOn = 0;
static bool switchedOn = false;

static void reject(uint8_t zone, Result r, Source src) {
    events::emit("command_rejected", "{\"zone\":%u,\"reden\":\"%s\",\"bron\":\"%s\"}",
                 zone + 1, resultName(r), sourceName(src));
}

static void finish(uint8_t zone, Source src, const char* reason) {
    Run& r = runs[zone];
    if (r.state == State::Open) {
        relays::setZone(zone, false);
        uint32_t secs = (millis() - r.openedAt) / 1000;
        events::emit("zone_closed", "{\"zone\":%u,\"duur_s\":%lu,\"reden\":\"%s\",\"bron\":\"%s\"}",
                     zone + 1, (unsigned long)secs, reason, sourceName(src));
    } else if (r.state == State::Pending) {
        events::emit("zone_closed", "{\"zone\":%u,\"duur_s\":0,\"reden\":\"%s\",\"bron\":\"%s\"}",
                     zone + 1, reason, sourceName(src));
    }
    r.state = State::Closed;
}

void begin() {
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        snprintf(cfg[i].name, sizeof(cfg[i].name), "Zone %u", i + 1);
        cfg[i].type = Type::Gazon;
        cfg[i].enabled = true;
        cfg[i].defaultDurationS = DEFAULT_DURATION_S;
        cfg[i].maxRuntimeS = DEFAULT_MAX_RUNTIME_S;
        runs[i] = Run{};
    }
}

Result open(uint8_t zone, uint32_t durationS, Source src) {
    if (zone >= ZONE_COUNT) {
        reject(zone, Result::InvalidZone, src);
        return Result::InvalidZone;
    }
    if (!cfg[zone].enabled) {
        reject(zone, Result::Disabled, src);
        return Result::Disabled;
    }

    const uint32_t maxMs = cfg[zone].maxRuntimeS * 1000;
    const uint32_t wantMs = durationS * 1000;
    Run& r = runs[zone];

    if (r.state == State::Open) {
        // Looptijd aanpassen, gerekend vanaf nu, maar nooit voorbij max looptijd.
        uint32_t elapsed = millis() - r.openedAt;
        uint32_t total = durationS == 0 ? maxMs : elapsed + wantMs;
        r.capped = total >= maxMs;
        r.runMs = r.capped ? maxMs : total;
        r.src = src;
        LOG("Zone %u: looptijd aangepast, nog %lu s", zone + 1, (unsigned long)remainingS(zone));
        return Result::Ok;
    }

    if (r.state == State::Closed && activeCount() >= globalSettings.maxConcurrent) {
        reject(zone, Result::LimitReached, src);
        return Result::LimitReached;
    }

    if (r.state == State::Closed) r.seq = nextSeq++;
    r.state = State::Pending;
    r.src = src;
    r.capped = durationS == 0 || wantMs >= maxMs;
    r.runMs = r.capped ? maxMs : wantMs;
    return Result::Ok;
}

bool close(uint8_t zone, Source src, const char* reason) {
    if (zone >= ZONE_COUNT || runs[zone].state == State::Closed) return false;
    finish(zone, src, reason);
    return true;
}

void closeAll(Source src, const char* reason) {
    for (uint8_t i = 0; i < ZONE_COUNT; i++) close(i, src, reason);
}

void emergencyStop(Source src) {
    relays::allOff();  // eerst fysiek dicht, dan pas administratie
    closeAll(src, "noodstop");
    events::emit("emergency_stop", "{\"bron\":\"%s\"}", sourceName(src));
    // TODO fase 5: lopende programma's en wachtrij afbreken
}

void loop() {
    const uint32_t now = millis();

    // Verlopen looptijden
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        Run& r = runs[i];
        if (r.state != State::Open || now - r.openedAt < r.runMs) continue;
        if (r.capped) {
            events::emit("max_runtime_reached", "{\"zone\":%u,\"max_s\":%lu}",
                         i + 1, (unsigned long)cfg[i].maxRuntimeS);
            finish(i, Source::System, "max_looptijd");
        } else {
            finish(i, Source::System, "timer");
        }
    }

    // Wachtende zones openen, één per keer met stagger ertussen (inschakelstroom)
    if (switchedOn && now - lastSwitchOn < globalSettings.staggerMs) return;
    int8_t first = -1;
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        if (runs[i].state == State::Pending && (first < 0 || runs[i].seq < runs[first].seq)) first = i;
    }
    if (first < 0) return;

    Run& r = runs[first];
    relays::setZone(first, true);
    r.state = State::Open;
    r.openedAt = now;
    lastSwitchOn = now;
    switchedOn = true;
    events::emit("zone_opened", "{\"zone\":%u,\"duur_s\":%lu,\"onbepaald\":%s,\"bron\":\"%s\"}",
                 first + 1, (unsigned long)(r.runMs / 1000), r.capped ? "true" : "false",
                 sourceName(r.src));
}

State state(uint8_t zone) { return zone < ZONE_COUNT ? runs[zone].state : State::Closed; }

uint32_t remainingS(uint8_t zone) {
    if (zone >= ZONE_COUNT) return 0;
    const Run& r = runs[zone];
    if (r.state == State::Pending) return r.runMs / 1000;
    if (r.state != State::Open) return 0;
    uint32_t elapsed = millis() - r.openedAt;
    return elapsed >= r.runMs ? 0 : (r.runMs - elapsed + 999) / 1000;
}

uint8_t activeCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < ZONE_COUNT; i++) n += runs[i].state != State::Closed;
    return n;
}

bool anyOpen() { return relays::mask() != 0; }

Config& config(uint8_t zone) { return cfg[zone < ZONE_COUNT ? zone : 0]; }
Settings& settings() { return globalSettings; }

const char* sourceName(Source s) {
    switch (s) {
        case Source::Web: return "web";
        case Source::Api: return "api";
        case Source::Mqtt: return "mqtt";
        case Source::Schema: return "schema";
        case Source::Knop: return "knop";
        case Source::Console: return "console";
        default: return "systeem";
    }
}

const char* resultName(Result r) {
    switch (r) {
        case Result::Ok: return "ok";
        case Result::InvalidZone: return "ongeldige_zone";
        case Result::Disabled: return "uitgeschakeld";
        case Result::LimitReached: return "limiet";
    }
    return "?";
}

}  // namespace zones
