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
    uint8_t program;  // 1-based; 0 = testcyclus
    Phase phase;
    uint8_t step;
    uint8_t zone;
    zones::Source src;
    uint32_t seq;
    uint32_t phaseAt;    // millis() begin huidige fase
    uint32_t startedAt;  // millis() begin run (na wachtrij)
};
static Run runs[MAX_RUNS];
static uint32_t nextSeq = 0;
static uint32_t queueSig = 0;
static int32_t lastMinuteKey = -1;

static const char* const DAY_NAMES[] = {"ma", "di", "wo", "do", "vr", "za", "zo"};

// ---------- hulpfuncties ----------

static const char* progName(uint8_t id) {
    if (id == 0) return "Testcyclus";
    return (id <= MAX_PROGRAMS && progs[id - 1].used) ? progs[id - 1].name : "?";
}

static uint32_t scaledDuration(uint8_t zone, uint16_t base) {
    const uint8_t typeFactor =
        zones::config(zone).type == zones::Type::Border ? cfg.factorBorder : cfg.factorGazon;
    return (uint32_t)base * cfg.seasonFactor * typeFactor / 10000;
}

// Stap idx van een run: zone + (geschaalde) duur. false = geen stap meer.
static bool stepAt(const Run& r, uint8_t idx, uint8_t& zone, uint32_t& dur) {
    if (r.program == 0) {  // testcyclus: elke ingeschakelde zone TEST_STEP_S, zonder factor
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
    events::emit("program_skipped", "{\"program\":%u,\"naam\":\"%s\",\"reden\":\"%s\"}", id, progName(id), reason);
    history::add(history::Action::ProgramSkipped, 0, id, 0, (uint8_t)src, reason);
}

static void finish(Run& r, const char* reason) {
    if (r.phase == Phase::Watering) zones::close(r.zone, zones::Source::System, reason);
    if (r.phase != Phase::Queued) {
        const uint32_t secs = (millis() - r.startedAt) / 1000;
        events::emit("program_finished", "{\"program\":%u,\"naam\":\"%s\",\"duur_s\":%lu,\"reden\":\"%s\"}",
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
    if (currentMode == Mode::Manual) return skipped(id, "handmatig", zones::Source::Schema);
    if (currentMode == Mode::Winter) return skipped(id, "winter", zones::Source::Schema);
    if (skipActive()) return skipped(id, "regenpauze", zones::Source::Schema);
    if (skipNext) {
        skipNext = false;
        storage::markDirty();
        return skipped(id, "overgeslagen", zones::Source::Schema);
    }
    RunResult r = enqueue(id, zones::Source::Schema);
    if (r == RunResult::AlreadyRunning) skipped(id, "loopt_al", zones::Source::Schema);
    else if (r != RunResult::Ok) skipped(id, "afgebroken", zones::Source::Schema);
}

static void schedulerTick() {
    struct tm lt;
    if (!ntp::localNow(lt)) return;
    const int32_t key = lt.tm_yday * 1440 + lt.tm_hour * 60 + lt.tm_min;
    if (key == lastMinuteKey) return;
    // Eerste geldige minuut na boot niet starten: na een (watchdog-)reboot binnen
    // dezelfde minuut zou een run anders dubbel starten.
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

    // Wachtrij → actief, oudste eerst
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
        events::emit("program_started", "{\"program\":%u,\"naam\":\"%s\",\"bron\":\"%s\"}", next->program,
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
                // Stappen overslaan die niets doen (factor 0, zone uit)
                while (stepAt(r, r.step, zone, dur) && (dur == 0 || !zones::config(zone).enabled)) r.step++;
                if (!stepAt(r, r.step, zone, dur)) {
                    finish(r, "klaar");
                    break;
                }
                if (zones::state(zone) != zones::State::Closed) break;            // zone bezet
                if (zones::activeCount() >= zones::settings().maxConcurrent) break;  // geen plek
                zones::Result res = zones::open(zone, dur, zones::Source::Schema);
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
                    finish(r, "klaar");
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

    // Wachtrij gewijzigd? (handtekening van programma-id's in wachtrij)
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
        events::emit("queue_changed", "{\"wachtrij\":[%s]}", list);
    }
}

// ---------- publieke API ----------

void begin() {
    for (Program& p : progs) p = Program{};
    for (Run& r : runs) r = Run{};
    zones::setStopHook([]() { abortAll("noodstop"); });
}

void loop() {
    schedulerTick();
    runnerTick();
}

Mode mode() { return currentMode; }

const char* modeName() {
    if (findRun(0)) return "test";
    switch (currentMode) {
        case Mode::Manual: return "handmatig";
        case Mode::Winter: return "winter";
        default: return "auto";
    }
}

static const char* plainModeName(Mode m) {
    return m == Mode::Manual ? "handmatig" : m == Mode::Winter ? "winter" : "auto";
}

bool parseMode(const char* s, Mode& out) {
    if (!strcmp(s, "auto")) out = Mode::Auto;
    else if (!strcmp(s, "handmatig")) out = Mode::Manual;
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
    events::emit("mode_changed", "{\"modus\":\"%s\",\"vorige\":\"%s\",\"bron\":\"%s\"}", plainModeName(m),
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
        case RunResult::Ok: return "Gestart of in wachtrij";
        case RunResult::NotFound: return "Onbekend programma";
        case RunResult::Empty: return "Programma heeft geen stappen";
        case RunResult::Winter: return "Wintermodus: alles geblokkeerd";
        case RunResult::AlreadyRunning: return "Programma loopt al of staat in de wachtrij";
        case RunResult::Full: return "Wachtrij vol";
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
        history::add(history::Action::SkipSet, 0, 0, 0, (uint8_t)src, "opgeheven");
    } else {
        if (!ntp::valid()) return false;
        skipUntil = ntp::now() + (time_t)hours * 3600;
        strlcpy(skipReason, reason ? reason : "", sizeof(skipReason));
        history::add(history::Action::SkipSet, 0, 0, hours * 3600, (uint8_t)src, "regenpauze");
    }
    storage::markDirty();
    web::markStatusDirty();
    return true;
}

void setSkipNext(const char* reason, zones::Source src) {
    skipNext = true;
    strlcpy(skipReason, reason ? reason : "", sizeof(skipReason));
    history::add(history::Action::SkipSet, 0, 0, 0, (uint8_t)src, "overgeslagen");
    storage::markDirty();
    web::markStatusDirty();
}

// ---------- programma's: JSON ----------

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
    if (!name[0] || strlen(name) >= sizeof(p.name)) return *err = "Naam: 1-23 tekens", false;
    strlcpy(p.name, name, sizeof(p.name));

    for (uint16_t& s : p.starts) s = NO_START;
    JsonArrayConst starts = in["starts"];
    if (starts.size() < 1 || starts.size() > MAX_STARTS) return *err = "Starttijden: 1-4", false;
    for (uint8_t i = 0; i < starts.size(); i++) {
        if (!parseHhmm(starts[i] | "", p.starts[i])) return *err = "Starttijd moet UU:MM zijn", false;
    }

    p.everyDays = in["every_days"] | 0;
    if (p.everyDays > 30) return *err = "Elke N dagen: 0-30", false;
    if (p.everyDays) {
        struct tm lt;
        p.anchorDay = ntp::localNow(lt) ? ntp::dayNumber(lt) : 0;
        if (!in["from"].isNull() && !parseDate(in["from"] | "", p.anchorDay)) {
            return *err = "Startdatum moet JJJJ-MM-DD zijn", false;
        }
    } else {
        for (JsonVariantConst d : in["days"].as<JsonArrayConst>()) {
            bool found = false;
            for (uint8_t i = 0; i < 7; i++) {
                if (!strcmp(d | "", DAY_NAMES[i])) {
                    p.days |= 1 << i;
                    found = true;
                }
            }
            if (!found) return *err = "Dagen: ma, di, wo, do, vr, za, zo", false;
        }
        if (!p.days) return *err = "Kies minstens één dag (of elke N dagen)", false;
    }

    JsonArrayConst steps = in["steps"];
    if (steps.size() > MAX_STEPS) return *err = "Maximaal 10 stappen", false;
    for (JsonObjectConst s : steps) {
        uint32_t zone = s["zone"] | 0u;
        uint32_t dur = s["duration_s"] | 0u;
        if (zone < 1 || zone > ZONE_COUNT) return *err = "Stap: zone 1-5", false;
        if (dur < 1 || dur > 4 * 3600) return *err = "Stap: duur 1 s – 4 uur", false;
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
    if (Run* r = findRun(id)) finish(*r, "afgebroken");
    progs[id - 1] = Program{};
    storage::markDirty();
    web::markStatusDirty();
}

// Volgende geplande start (epoch) binnen 14 dagen; 0 als er geen is.
static time_t nextStart(const Program& p) {
    struct tm now;
    if (!p.used || !p.enabled || !p.stepCount || !ntp::localNow(now)) return 0;
    const uint16_t nowMin = now.tm_hour * 60 + now.tm_min;
    for (uint8_t d = 0; d <= 14; d++) {
        struct tm day = now;
        day.tm_mday += d;
        day.tm_hour = 12;  // normaliseren rond het middaguur (zomertijd-veilig)
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
    o["total_s"] = total;  // met factoren
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

// ---------- instellingen + status ----------

Settings& settings() { return cfg; }

bool settingsFromJson(JsonObjectConst in, Settings& out, const char** err) {
    Settings next = out;
    struct Field { const char* key; uint8_t* dst; };
    for (const Field& f : {Field{"season_factor", &next.seasonFactor}, Field{"factor_gazon", &next.factorGazon},
                           Field{"factor_border", &next.factorBorder}}) {
        if (in[f.key].isNull()) continue;
        int v = in[f.key] | -1;
        if (v < 0 || v > 200) return *err = "Factoren: 0-200 %", false;
        *f.dst = v;
    }
    if (!in["zone_pause_s"].isNull()) {
        int v = in["zone_pause_s"] | -1;
        if (v < 0 || v > 600) return *err = "Pauze tussen zones: 0-600 s", false;
        next.zonePauseS = v;
    }
    out = next;
    return true;
}

void settingsToJson(JsonObject o) {
    o["season_factor"] = cfg.seasonFactor;
    o["factor_gazon"] = cfg.factorGazon;
    o["factor_border"] = cfg.factorBorder;
    o["zone_pause_s"] = cfg.zonePauseS;
}

void statusJson(JsonObject o) {
    char buf[24];
    o["mode"] = modeName();
    o["factor"] = cfg.seasonFactor;
    o["factor_gazon"] = cfg.factorGazon;
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

    // Eerstvolgende run over alle programma's
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

// ---------- opslag ----------

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
        if (!progs[i].used) continue;  // lege plek = {}
        programBody(o, progs[i], true);
    }
}

bool importConfig(JsonObjectConst root, const char** err) {
    Settings next = cfg;
    Mode m = currentMode;
    JsonObjectConst s = root["schedule"];
    if (!s.isNull()) {
        if (!settingsFromJson(s, next, err)) return false;
        if (!s["mode"].isNull() && !parseMode(s["mode"] | "", m)) return *err = "Onbekende modus", false;
    }

    // Programma's eerst allemaal valideren (in een tijdelijke kopie op de heap)
    JsonArrayConst arr = root["programs"];
    std::unique_ptr<Program[]> parsed(new Program[MAX_PROGRAMS]());
    for (uint8_t i = 0; i < MAX_PROGRAMS && i < arr.size(); i++) {
        JsonObjectConst o = arr[i];
        if (o.isNull() || o.size() == 0) continue;
        if (!fromJson(o, parsed[i], err)) return false;
    }

    cfg = next;
    if (!arr.isNull()) {
        abortAll("afgebroken");
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
