#include "zones.h"
#include "config.h"
#include "relays.h"
#include "events.h"
#include "log.h"
#include "history.h"

namespace zones {

struct Run {
    State state;
    Source src;
    bool capped;         // runtime capped by maxRuntimeS (indefinite or requested too long)
    uint32_t seq;        // request order, for the pending zones
    uint32_t openedAt;   // millis() when the relay opened
    uint32_t runMs;      // total runtime from openedAt
};

static Config cfg[ZONE_COUNT];
static Run runs[ZONE_COUNT];
static Settings globalSettings = {DEFAULT_MAX_CONCURRENT, DEFAULT_STAGGER_MS};
static uint32_t nextSeq = 0;
static uint32_t lastSwitchOn = 0;
static bool switchedOn = false;
static bool locked = false;
static std::function<void()> stopHook;

static void reject(uint8_t zone, Result r, Source src) {
    events::emit("command_rejected", "{\"zone\":%u,\"reason\":\"%s\",\"source\":\"%s\"}",
                 zone + 1, resultName(r), sourceName(src));
    history::add(history::Action::Rejected, zone + 1, 0, 0, (uint8_t)src, resultName(r));
}

static void finish(uint8_t zone, Source src, const char* reason) {
    Run& r = runs[zone];
    if (r.state == State::Open) {
        relays::setZone(zone, false);
        uint32_t secs = (millis() - r.openedAt) / 1000;
        events::emit("zone_closed", "{\"zone\":%u,\"duration_s\":%lu,\"reason\":\"%s\",\"source\":\"%s\"}",
                     zone + 1, (unsigned long)secs, reason, sourceName(src));
        history::add(history::Action::ZoneClosed, zone + 1, 0, secs, (uint8_t)src, reason);
        history::addRuntime(zone, secs);
    } else if (r.state == State::Pending) {
        events::emit("zone_closed", "{\"zone\":%u,\"duration_s\":0,\"reason\":\"%s\",\"source\":\"%s\"}",
                     zone + 1, reason, sourceName(src));
    }
    r.state = State::Closed;
}

void begin() {
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        snprintf(cfg[i].name, sizeof(cfg[i].name), "Zone %u", i + 1);
        cfg[i].type = Type::Lawn;
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
    if (locked) {
        reject(zone, Result::Locked, src);
        return Result::Locked;
    }
    if (!cfg[zone].enabled) {
        reject(zone, Result::Disabled, src);
        return Result::Disabled;
    }

    const uint32_t maxMs = cfg[zone].maxRuntimeS * 1000;
    const uint32_t wantMs = durationS * 1000;
    Run& r = runs[zone];

    if (r.state == State::Open) {
        // Adjust the runtime, counted from now, but never beyond the max runtime.
        uint32_t elapsed = millis() - r.openedAt;
        uint32_t total = durationS == 0 ? maxMs : elapsed + wantMs;
        r.capped = total >= maxMs;
        r.runMs = r.capped ? maxMs : total;
        r.src = src;
        LOG("Zone %u: runtime adjusted, %lu s left", zone + 1, (unsigned long)remainingS(zone));
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
    relays::allOff();  // physically closed first, bookkeeping second
    closeAll(src, "emergency_stop");
    events::emit("emergency_stop", "{\"source\":\"%s\"}", sourceName(src));
    history::add(history::Action::EmergencyStop, 0, 0, 0, (uint8_t)src, "emergency_stop");
    if (stopHook) stopHook();  // abort running programs and the queue
}

void setLocked(bool l) { locked = l; }
void setStopHook(std::function<void()> hook) { stopHook = hook; }

void loop() {
    const uint32_t now = millis();

    // Expired runtimes
    for (uint8_t i = 0; i < ZONE_COUNT; i++) {
        Run& r = runs[i];
        if (r.state != State::Open || now - r.openedAt < r.runMs) continue;
        if (r.capped) {
            events::emit("max_runtime_reached", "{\"zone\":%u,\"max_s\":%lu}",
                         i + 1, (unsigned long)cfg[i].maxRuntimeS);
            history::add(history::Action::MaxRuntime, i + 1, 0, cfg[i].maxRuntimeS, (uint8_t)Source::System, "max_runtime");
            finish(i, Source::System, "max_runtime");
        } else {
            finish(i, Source::System, "timer");
        }
    }

    // Open pending zones one at a time, with stagger in between (inrush current)
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
    events::emit("zone_opened", "{\"zone\":%u,\"duration_s\":%lu,\"indefinite\":%s,\"source\":\"%s\"}",
                 first + 1, (unsigned long)(r.runMs / 1000), r.capped ? "true" : "false",
                 sourceName(r.src));
    history::add(history::Action::ZoneOpened, first + 1, 0, r.runMs / 1000, (uint8_t)r.src, "");
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
        case Source::Schedule: return "schedule";
        case Source::Button: return "button";
        case Source::Console: return "console";
        default: return "system";
    }
}

const char* resultName(Result r) {
    switch (r) {
        case Result::Ok: return "ok";
        case Result::InvalidZone: return "invalid_zone";
        case Result::Disabled: return "disabled";
        case Result::LimitReached: return "limit";
        case Result::Locked: return "winter";
    }
    return "?";
}

}  // namespace zones
