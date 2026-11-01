#include "programs.h"
#include "ntp.h"
#include "events.h"
#include "history.h"
#include "storage.h"
#include "web.h"
#include "log.h"
#include <memory>

namespace programs {

static Program progs[MAX_PROGRAMS];
static Settings cfg = {100, 100, 100, DEFAULT_ZONE_PAUSE_S};
static Mode currentMode = Mode::Auto;

static time_t skipUntil = 0;
static bool skipNext = false;
static char skipReason[48] = "";

enum class Phase : uint8_t { Queued, Waiting, Watering, Pause };
struct Run {
    bool used;
    uint8_t program;  // 1-based; 0 = test cycle
    Phase phase;
    uint8_t step;
    uint8_t zone;
    zones::Source src;
    uint32_t seq;
    uint32_t phaseAt;    // millis() at start of current phase
    uint32_t startedAt;  // millis() at start of run (after the queue)
};
static Run runs[MAX_RUNS];
static uint32_t nextSeq = 0;
static uint32_t queueSig = 0;
static int32_t lastMinuteKey = -1;

static const char* const DAY_NAMES[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
// Legacy (Dutch) aliases, accepted on input only so older config.json files keep loading.
static const char* const LEGACY_DAY_NAMES[] = {"ma", "di", "wo", "do", "vr", "za", "zo"};

// ---------- helpers ----------

static const char* progName(uint8_t id) {
    if (id == 0) return "Test cycle";
    return (id <= MAX_PROGRAMS && progs[id - 1].used) ? progs[id - 1].name : "?";
}

static uint32_t scaledDuration(uint8_t zone, uint16_t base) {
    const uint8_t typeFactor =
        zones::config(zone).type == zones::Type::Border ? cfg.factorBorder : cfg.factorLawn;
    return (uint32_t)base * cfg.seasonFactor * typeFactor / 10000;
}

// Step idx of a run: zone + (scaled) duration. false = no more steps.
static bool stepAt(const Run& r, uint8_t idx, uint8_t& zone, uint32_t& dur) {
    if (r.program == 0) {  // test cycle: every enabled zone TEST_STEP_S, without factor
        uint8_t n = 0;
        for (uint8_t z = 0; z < ZONE_COUNT; z++) {
            if (!zones::config(z).enabled) continue;
            if (n++ == idx) {
                zone = z;
                dur = TEST_STEP_S;
                return true;
            }
        }
        return false;
    }
    const Program& p = progs[r.program - 1];
    if (idx >= p.stepCount) return false;
    zone = p.steps[idx].zone;
    dur = scaledDuration(zone, p.steps[idx].durationS);
    return true;
}

static bool dayMatches(const Program& p, const struct tm& t) {
    if (p.everyDays) {
        int32_t diff = ntp::dayNumber(t) - p.anchorDay;
        return diff >= 0 && diff % p.everyDays == 0;
    }
    return p.days & (1 << ((t.tm_wday + 6) % 7));
}

static Run* findRun(uint8_t program) {
    for (Run& r : runs) if (r.used && r.program == program) return &r;
    return nullptr;
}

static Run* freeRun() {
    for (Run& r : runs) if (!r.used) return &r;
    return nullptr;
}

static void skipped(uint8_t id, const char* reason, zones::Source src) {
    events::emit("program_skipped", "{\"program\":%u,\"name\":\"%s\",\"reason\":\"%s\"}", id, progName(id), reason);
    history::add(history::Action::ProgramSkipped, 0, id, 0, (uint8_t)src, reason);
}

static void finish(Run& r, const char* reason) {
    if (r.phase == Phase::Watering) zones::close(r.zone, zones::Source::System, reason);
    if (r.phase != Phase::Queued) {
        const uint32_t secs = (millis() - r.startedAt) / 1000;
        events::emit("program_finished", "{\"program\":%u,\"name\":\"%s\",\"duration_s\":%lu,\"reason\":\"%s\"}",
                     r.program, progName(r.program), (unsigned long)secs, reason);
        history::add(history::Action::ProgramFinished, 0, r.program ? r.program : history::PROGRAM_TEST, secs, (uint8_t)r.src, reason);
    }
    r.used = false;
}

static RunResult enqueue(uint8_t id, zones::Source src) {
    if (currentMode == Mode::Winter) return RunResult::Winter;
    if (findRun(id)) return RunResult::AlreadyRunning;
    Run* r = freeRun();
    if (!r) return RunResult::Full;
    *r = Run{};
    r->used = true;
    r->program = id;
    r->phase = Phase::Queued;
    r->src = src;
    r->seq = nextSeq++;
    return RunResult::Ok;
}

// ---------- scheduler ----------

static void trigger(uint8_t id) {
    if (currentMode == Mode::Manual) return skipped(id, "manual", zones::Source::Schedule);
    if (currentMode == Mode::Winter) return skipped(id, "winter", zones::Source::Schedule);
    if (skipActive()) return skipped(id, "rain_delay", zones::Source::Schedule);
    if (skipNext) {
        skipNext = false;
        storage::markDirty();
        return skipped(id, "skipped", zones::Source::Schedule);
    }
    RunResult r = enqueue(id, zones::Source::Schedule);
    if (r == RunResult::AlreadyRunning) skipped(id, "already_running", zones::Source::Schedule);
    else if (r != RunResult::Ok) skipped(id, "aborted", zones::Source::Schedule);
}

static void schedulerTick() {
    struct tm lt;
    if (!ntp::localNow(lt)) return;
    const int32_t key = lt.tm_yday * 1440 + lt.tm_hour * 60 + lt.tm_min;
    if (key == lastMinuteKey) return;
    // Don't start in the first valid minute after boot: after a (watchdog) reboot within
    // the same minute a run would otherwise start twice.
    const bool first = lastMinuteKey < 0;
    lastMinuteKey = key;
    if (first) return;

    const uint16_t minute = lt.tm_hour * 60 + lt.tm_min;
    for (uint8_t i = 0; i < MAX_PROGRAMS; i++) {
        const Program& p = progs[i];
        if (!p.used || !p.enabled || !p.stepCount || !dayMatches(p, lt)) continue;
        for (uint16_t s : p.starts) {
            if (s == minute) {
                trigger(i + 1);
                break;
            }
        }
    }
}

// ---------- runner ----------

static void runnerTick() {
    const uint32_t now = millis();
    const uint8_t maxRuns = zones::settings().maxConcurrent;

    uint8_t active = 0;
    for (const Run& r : runs) active += r.used && r.phase != Phase::Queued;

    // Queue → active, oldest first
    while (active < maxRuns) {
        Run* next = nullptr;
        for (Run& r : runs) {
            if (r.used && r.phase == Phase::Queued && (!next || r.seq < next->seq)) next = &r;
        }
        if (!next) break;
        next->phase = Phase::Waiting;
        next->startedAt = now;
        next->step = 0;
        active++;
        events::emit("program_started", "{\"program\":%u,\"name\":\"%s\",\"source\":\"%s\"}", next->program,
                     progName(next->program), zones::sourceName(next->src));
        history::add(history::Action::ProgramStarted, 0, next->program ? next->program : history::PROGRAM_TEST, 0, (uint8_t)next->src, "");
    }

    for (Run& r : runs) {
        if (!r.used) continue;
        uint8_t zone;
        uint32_t dur;
        switch (r.phase) {
            case Phase::Queued:
                break;

            case Phase::Waiting: {
                // Skip steps that do nothing (factor 0, zone disabled)
                while (stepAt(r, r.step, zone, dur) && (dur == 0 || !zones::config(zone).enabled)) r.step++;
                if (!stepAt(r, r.step, zone, dur)) {
                    finish(r, "done");
                    break;
                }
                if (zones::state(zone) != zones::State::Closed) break;            // zone busy
                if (zones::activeCount() >= zones::settings().maxConcurrent) break;  // no slot
                zones::Result res = zones::open(zone, dur, zones::Source::Schedule);
                if (res == zones::Result::Ok) {
                    r.phase = Phase::Watering;
                    r.zone = zone;
                    r.phaseAt = now;
                } else if (res == zones::Result::Disabled) {
                    r.step++;
                }
                break;
            }

            case Phase::Watering:
                if (zones::state(r.zone) != zones::State::Closed) break;
                r.step++;
                if (!stepAt(r, r.step, zone, dur)) {
                    finish(r, "done");
                } else {
                    r.phase = Phase::Pause;
                    r.phaseAt = now;
                }
                break;

            case Phase::Pause:
                if (now - r.phaseAt >= (uint32_t)cfg.zonePauseS * 1000) r.phase = Phase::Waiting;
                break;
        }
    }

    // Queue changed? (signature of the program IDs in the queue)
    uint32_t sig = 0;
    for (const Run& r : runs) {
        if (r.used && r.phase == Phase::Queued) sig = sig * 31 + r.program + 1;
    }
    if (sig != queueSig) {
        queueSig = sig;
        char list[40] = "";
        size_t n = 0;
        for (const Run& r : runs) {
            if (r.used && r.phase == Phase::Queued) n += snprintf(list + n, sizeof(list) - n, "%s%u", n ? "," : "", r.program);
        }
        events::emit("queue_changed", "{\"queue\":[%s]}", list);
    }
}

// ---------- public API ----------

void begin() {
    for (Program& p : progs) p = Program{};
    for (Run& r : runs) r = Run{};
    zones::setStopHook([]() { abortAll("emergency_stop"); });
}

void loop() {
    schedulerTick();
    runnerTick();
}

Mode mode() { return currentMode; }

const char* modeName() {
    if (findRun(0)) return "test";
    switch (currentMode) {
        case Mode::Manual: return "manual";
        case Mode::Winter: return "winter";
        default: return "auto";
    }
}

static const char* plainModeName(Mode m) {
    return m == Mode::Manual ? "manual" : m == Mode::Winter ? "winter" : "auto";
}

bool parseMode(const char* s, Mode& out) {
    if (!strcmp(s, "auto")) out = Mode::Auto;
    // "handmatig" is the legacy (Dutch) alias for "manual", so older config.json files keep loading.
    else if (!strcmp(s, "manual") || !strcmp(s, "handmatig")) out = Mode::Manual;
    else if (!strcmp(s, "winter")) out = Mode::Winter;
    else return false;
    return true;
}

void setMode(Mode m, zones::Source src) {
    if (m == currentMode) return;
    const Mode prev = currentMode;
    currentMode = m;
    zones::setLocked(m == Mode::Winter);
    if (m == Mode::Winter) {
        abortAll("winter");
        zones::closeAll(src, "winter");
    }
    events::emit("mode_changed", "{\"mode\":\"%s\",\"previous\":\"%s\",\"source\":\"%s\"}", plainModeName(m),
                 plainModeName(prev), zones::sourceName(src));
    history::add(history::Action::ModeChanged, 0, 0, 0, (uint8_t)src, plainModeName(m));
    storage::markDirty();
}

RunResult runNow(uint8_t id, zones::Source src) {
    if (id > MAX_PROGRAMS || (id && !progs[id - 1].used)) return RunResult::NotFound;
    if (id && !progs[id - 1].stepCount) return RunResult::Empty;
    return enqueue(id, src);
}

const char* runResultMessage(RunResult r) {
    switch (r) {
        case RunResult::Ok: return "Started or queued";
        case RunResult::NotFound: return "Unknown program";
        case RunResult::Empty: return "Program has no steps";
        case RunResult::Winter: return "Winter mode: everything blocked";
        case RunResult::AlreadyRunning: return "Program is already running or queued";
        case RunResult::Full: return "Queue full";
    }
    return "?";
}

void abortAll(const char* reason) {
    for (Run& r : runs) if (r.used) finish(r, reason);
}

bool skipActive() { return skipUntil && ntp::valid() && ntp::now() < skipUntil; }

bool setSkipHours(uint32_t hours, const char* reason, zones::Source src) {
    if (hours == 0) {
        skipUntil = 0;
        skipNext = false;
        skipReason[0] = '\0';
        history::add(history::Action::SkipSet, 0, 0, 0, (uint8_t)src, "cleared");
    } else {
        if (!ntp::valid()) return false;
        skipUntil = ntp::now() + (time_t)hours * 3600;
        strlcpy(skipReason, reason ? reason : "", sizeof(skipReason));
        history::add(history::Action::SkipSet, 0, 0, hours * 3600, (uint8_t)src, "rain_delay");
    }
    storage::markDirty();
    web::markStatusDirty();
    return true;
}

void setSkipNext(const char* reason, zones::Source src) {
    skipNext = true;
    strlcpy(skipReason, reason ? reason : "", sizeof(skipReason));
    history::add(history::Action::SkipSet, 0, 0, 0, (uint8_t)src, "skipped");
    storage::markDirty();
    web::markStatusDirty();
}

// ---------- programs: JSON ----------

bool exists(uint8_t id) { return id >= 1 && id <= MAX_PROGRAMS && progs[id - 1].used; }

int8_t freeId() {
    for (uint8_t i = 0; i < MAX_PROGRAMS; i++) if (!progs[i].used) return i + 1;
    return 0;
}

static bool parseHhmm(const char* s, uint16_t& out) {
    unsigned h, m;
    char extra;
    if (!s || sscanf(s, "%u:%u%c", &h, &m, &extra) != 2 || h > 23 || m > 59) return false;
    out = h * 60 + m;
    return true;
}

static bool parseDate(const char* s, int32_t& day) {
    unsigned y, mo, d;
    char extra;
    if (!s || sscanf(s, "%u-%u-%u%c", &y, &mo, &d, &extra) != 3 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    day = ntp::dayNumber(t);
    return true;
}

static void formatDay(int32_t day, char* buf, size_t len) {
    time_t t = (time_t)day * 86400;
    struct tm g;
    gmtime_r(&t, &g);
    strftime(buf, len, "%Y-%m-%d", &g);
}

bool fromJson(JsonObjectConst in, Program& out, const char** err) {
    Program p = {};
    p.used = true;
    p.enabled = in["enabled"] | true;

    const char* name = in["name"] | "";
    if (!name[0] || strlen(name) >= sizeof(p.name)) return *err = "Name: 1-23 characters", false;
    strlcpy(p.name, name, sizeof(p.name));

    for (uint16_t& s : p.starts) s = NO_START;
    JsonArrayConst starts = in["starts"];
    if (starts.size() < 1 || starts.size() > MAX_STARTS) return *err = "Start times: 1-4", false;
    for (uint8_t i = 0; i < starts.size(); i++) {
        if (!parseHhmm(starts[i] | "", p.starts[i])) return *err = "Start time must be HH:MM", false;
    }

    p.everyDays = in["every_days"] | 0;
    if (p.everyDays > 30) return *err = "Every N days: 0-30", false;
    if (p.everyDays) {
        struct tm lt;
        p.anchorDay = ntp::localNow(lt) ? ntp::dayNumber(lt) : 0;
        if (!in["from"].isNull() && !parseDate(in["from"] | "", p.anchorDay)) {
            return *err = "Start date must be YYYY-MM-DD", false;
        }
    } else {
        for (JsonVariantConst d : in["days"].as<JsonArrayConst>()) {
            bool found = false;
            for (uint8_t i = 0; i < 7; i++) {
                const char* s = d | "";
                if (!strcmp(s, DAY_NAMES[i]) || !strcmp(s, LEGACY_DAY_NAMES[i])) {
                    p.days |= 1 << i;
                    found = true;
                }
            }
            if (!found) return *err = "Days: mon, tue, wed, thu, fri, sat, sun", false;
        }
        if (!p.days) return *err = "Pick at least one day (or every N days)", false;
    }

    JsonArrayConst steps = in["steps"];
    if (steps.size() > MAX_STEPS) return *err = "At most 10 steps", false;
    for (JsonObjectConst s : steps) {
        uint32_t zone = s["zone"] | 0u;
        uint32_t dur = s["duration_s"] | 0u;
        if (zone < 1 || zone > ZONE_COUNT) return *err = "Step: zone 1-5", false;
        if (dur < 1 || dur > 4 * 3600) return *err = "Step: duration 1 s – 4 h", false;
        p.steps[p.stepCount++] = {(uint8_t)(zone - 1), (uint16_t)dur};
    }

    out = p;
    return true;
}

void store(uint8_t id, const Program& p) {
    if (id < 1 || id > MAX_PROGRAMS) return;
    progs[id - 1] = p;
    storage::markDirty();
    web::markStatusDirty();
}

void remove(uint8_t id) {
    if (!exists(id)) return;
    if (Run* r = findRun(id)) finish(*r, "aborted");
    progs[id - 1] = Program{};
    storage::markDirty();
    web::markStatusDirty();
}

// Next scheduled start (epoch) within 14 days; 0 if there is none.
static time_t nextStart(const Program& p) {
    struct tm now;
    if (!p.used || !p.enabled || !p.stepCount || !ntp::localNow(now)) return 0;
    const uint16_t nowMin = now.tm_hour * 60 + now.tm_min;
    for (uint8_t d = 0; d <= 14; d++) {
        struct tm day = now;
        day.tm_mday += d;
        day.tm_hour = 12;  // normalise around noon (DST-safe)
        day.tm_min = day.tm_sec = 0;
        day.tm_isdst = -1;
        mktime(&day);
        if (!dayMatches(p, day)) continue;
        uint16_t best = NO_START;
        for (uint16_t s : p.starts) {
            if (s == NO_START || (d == 0 && s <= nowMin)) continue;
            if (s < best) best = s;
        }
        if (best == NO_START) continue;
        day.tm_hour = best / 60;
        day.tm_min = best % 60;
        day.tm_isdst = -1;
        return mktime(&day);
    }
    return 0;
}

static void programBody(JsonObject o, const Program& p, bool forStorage) {
    o["name"] = p.name;
    o["enabled"] = p.enabled;
    JsonArray starts = o["starts"].to<JsonArray>();
    char buf[20];
    for (uint16_t s : p.starts) {
        if (s == NO_START) continue;
        snprintf(buf, sizeof(buf), "%02u:%02u", s / 60, s % 60);
        starts.add(buf);
    }
    o["every_days"] = p.everyDays;
    if (p.everyDays) {
        formatDay(p.anchorDay, buf, sizeof(buf));
        o["from"] = buf;
    }
    JsonArray days = o["days"].to<JsonArray>();
    for (uint8_t i = 0; i < 7; i++) if (p.days & (1 << i)) days.add(DAY_NAMES[i]);
    JsonArray steps = o["steps"].to<JsonArray>();
    uint32_t total = 0;
    for (uint8_t i = 0; i < p.stepCount; i++) {
        JsonObject s = steps.add<JsonObject>();
        s["zone"] = p.steps[i].zone + 1;
        s["duration_s"] = p.steps[i].durationS;
        total += scaledDuration(p.steps[i].zone, p.steps[i].durationS);
    }
    if (forStorage) return;
    o["total_s"] = total;  // with factors
    time_t next = nextStart(p);
    if (next) {
        ntp::format(next, buf, sizeof(buf));
        o["next_run"] = buf;
    } else {
        o["next_run"] = nullptr;
    }
    o["running"] = findRun((uint8_t)(&p - progs + 1)) != nullptr;
}

void toJson(JsonObject o, uint8_t id) {
    if (!exists(id)) return;
    o["id"] = id;
    programBody(o, progs[id - 1], false);
}

void listJson(JsonArray arr) {
    for (uint8_t id = 1; id <= MAX_PROGRAMS; id++) {
        if (exists(id)) toJson(arr.add<JsonObject>(), id);
    }
}

// ---------- settings + status ----------

Settings& settings() { return cfg; }

bool settingsFromJson(JsonObjectConst in, Settings& out, const char** err) {
    Settings next = out;
    struct Field { const char* key; uint8_t* dst; };
    // "factor_gazon" is the legacy (Dutch) alias for "factor_lawn", so older config.json files
    // keep loading. It comes before "factor_lawn" so the new key wins when both are present.
    for (const Field& f : {Field{"season_factor", &next.seasonFactor}, Field{"factor_gazon", &next.factorLawn},
                           Field{"factor_lawn", &next.factorLawn}, Field{"factor_border", &next.factorBorder}}) {
        if (in[f.key].isNull()) continue;
        int v = in[f.key] | -1;
        if (v < 0 || v > 200) return *err = "Factors: 0-200 %", false;
        *f.dst = v;
    }
    if (!in["zone_pause_s"].isNull()) {
        int v = in["zone_pause_s"] | -1;
        if (v < 0 || v > 600) return *err = "Pause between zones: 0-600 s", false;
        next.zonePauseS = v;
    }
    out = next;
    return true;
}

void settingsToJson(JsonObject o) {
    o["season_factor"] = cfg.seasonFactor;
    o["factor_lawn"] = cfg.factorLawn;
    o["factor_border"] = cfg.factorBorder;
    o["zone_pause_s"] = cfg.zonePauseS;
}

void statusJson(JsonObject o) {
    char buf[24];
    o["mode"] = modeName();
    o["factor"] = cfg.seasonFactor;
    o["factor_lawn"] = cfg.factorLawn;
    o["factor_border"] = cfg.factorBorder;

    o["time_valid"] = ntp::valid();
    if (ntp::valid()) {
        ntp::format(ntp::now(), buf, sizeof(buf));
        o["time"] = buf;
    }

    JsonObject skip = o["skip"].to<JsonObject>();
    skip["active"] = skipActive();
    skip["next"] = skipNext;
    if (skipActive()) {
        ntp::format(skipUntil, buf, sizeof(buf));
        skip["until"] = buf;
    }
    if (skipActive() || skipNext) skip["reason"] = skipReason;

    JsonArray active = o["runs"].to<JsonArray>();
    JsonArray queue = o["queue"].to<JsonArray>();
    for (const Run& r : runs) {
        if (!r.used) continue;
        if (r.phase == Phase::Queued) {
            JsonObject q = queue.add<JsonObject>();
            q["program"] = r.program;
            q["name"] = progName(r.program);
            continue;
        }
        JsonObject a = active.add<JsonObject>();
        a["program"] = r.program;
        a["name"] = progName(r.program);
        a["step"] = r.step + 1;
        uint8_t steps = 0, z;
        uint32_t d;
        while (stepAt(r, steps, z, d)) steps++;
        a["steps"] = steps;
        switch (r.phase) {
            case Phase::Watering:
                a["state"] = "running";
                a["zone"] = r.zone + 1;
                a["remaining_s"] = zones::remainingS(r.zone);
                break;
            case Phase::Pause: {
                a["state"] = "pause";
                uint32_t el = (millis() - r.phaseAt) / 1000;
                a["remaining_s"] = el >= cfg.zonePauseS ? 0 : cfg.zonePauseS - el;
                break;
            }
            default:
                a["state"] = "waiting";
        }
    }

    // Next upcoming run across all programs
    time_t best = 0;
    uint8_t bestId = 0;
    for (uint8_t i = 0; i < MAX_PROGRAMS; i++) {
        time_t t = nextStart(progs[i]);
        if (t && (!best || t < best)) {
            best = t;
            bestId = i + 1;
        }
    }
    if (best) {
        JsonObject n = o["next_run"].to<JsonObject>();
        n["program"] = bestId;
        n["name"] = progs[bestId - 1].name;
        ntp::format(best, buf, sizeof(buf));
        n["at"] = buf;
    } else {
        o["next_run"] = nullptr;
    }
}

// ---------- persistence ----------

void exportConfig(JsonObject root) {
    JsonObject s = root["schedule"].to<JsonObject>();
    settingsToJson(s);
    s["mode"] = plainModeName(currentMode);
    s["skip_until"] = (uint32_t)skipUntil;
    s["skip_next"] = skipNext;
    s["skip_reason"] = skipReason;
    JsonArray arr = root["programs"].to<JsonArray>();
    for (uint8_t i = 0; i < MAX_PROGRAMS; i++) {
        JsonObject o = arr.add<JsonObject>();
        if (!progs[i].used) continue;  // empty slot = {}
        programBody(o, progs[i], true);
    }
}

bool importConfig(JsonObjectConst root, const char** err) {
    Settings next = cfg;
    Mode m = currentMode;
    JsonObjectConst s = root["schedule"];
    if (!s.isNull()) {
        if (!settingsFromJson(s, next, err)) return false;
        if (!s["mode"].isNull() && !parseMode(s["mode"] | "", m)) return *err = "Unknown mode", false;
    }

    // Validate all programs first (in a temporary copy on the heap)
    JsonArrayConst arr = root["programs"];
    std::unique_ptr<Program[]> parsed(new Program[MAX_PROGRAMS]());
    for (uint8_t i = 0; i < MAX_PROGRAMS && i < arr.size(); i++) {
        JsonObjectConst o = arr[i];
        if (o.isNull() || o.size() == 0) continue;
        if (!fromJson(o, parsed[i], err)) return false;
    }

    cfg = next;
    if (!arr.isNull()) {
        abortAll("aborted");
        for (uint8_t i = 0; i < MAX_PROGRAMS; i++) progs[i] = parsed[i];
    }
    if (!s.isNull()) {
        skipUntil = s["skip_until"] | 0u;
        skipNext = s["skip_next"] | false;
        strlcpy(skipReason, s["skip_reason"] | "", sizeof(skipReason));
        if (m != currentMode) {
            currentMode = m;
            zones::setLocked(m == Mode::Winter);
        }
    }
    return true;
}

}  // namespace programs
