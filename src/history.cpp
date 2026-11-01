#include "history.h"
#include <LittleFS.h>
#include "config.h"
#include "ntp.h"
#include "zones.h"
#include "log.h"

namespace history {

// Order is fixed: /log.bin stores indices into this table. Only append, never reorder.
static const char* const REASONS[] = {
    "",          "timer",   "max_runtime",     "manual",  "emergency_stop", "disabled",
    "ota",       "restart", "limit",           "winter",  "done",           "aborted",
    "rain_delay", "skipped", "already_running", "no_time", "auto",          "test",
    "invalid_zone", "update_in_progress", "no_steps", "factor_zero", "cleared", "other",
};
static const uint8_t REASON_COUNT = sizeof(REASONS) / sizeof(REASONS[0]);

static const char* const ACTIONS[] = {
    "boot", "zone_opened", "zone_closed", "command_rejected", "max_runtime_reached", "emergency_stop",
    "program_started", "program_finished", "program_skipped", "mode_changed", "skip_set", "update",
};
static const char* const SOURCES[] = {"web", "api", "mqtt", "schedule", "button", "console", "system"};

struct Entry {
    uint32_t t;         // epoch; 0 = clock not set yet
    uint32_t duration;  // seconds
    uint8_t action, zone, program, source, reason;
    uint8_t pad[3];
};
static_assert(sizeof(Entry) == 16, "Entry layout is also stored in /log.bin");

struct LogFile {
    uint32_t magic;
    uint16_t head, count;
    Entry entries[LOG_ENTRIES];
};
static LogFile logbuf;

struct Counters {
    uint32_t total, week, season;
};
struct StatsFile {
    uint32_t magic;
    int32_t weekKey;     // day number of the Monday of the current week
    int32_t seasonYear;  // calendar year
    Counters zone[ZONE_COUNT];
};
static StatsFile stats;

static const uint32_t LOG_MAGIC = 0x534C4731;    // "SLG1"
static const uint32_t STATS_MAGIC = 0x53535431;  // "SST1"
static const char LOG_PATH[] = "/log.bin";
static const char STATS_PATH[] = "/stats.bin";

static bool dirty = false;
static uint32_t lastSave = 0;

static uint8_t reasonCode(const char* r) {
    if (!r) return 0;
    for (uint8_t i = 0; i < REASON_COUNT; i++) {
        if (!strcmp(REASONS[i], r)) return i;
    }
    return REASON_COUNT - 1;  // "other"
}

// Roll over week/season when we're in a new week/year (only with a valid clock).
static void rollCounters() {
    struct tm lt;
    if (!ntp::localNow(lt)) return;
    const int32_t day = ntp::dayNumber(lt);
    const int32_t monday = day - (lt.tm_wday + 6) % 7;
    const int32_t year = lt.tm_year + 1900;
    if (stats.weekKey != monday) {
        if (stats.weekKey) for (auto& c : stats.zone) c.week = 0;
        stats.weekKey = monday;
        dirty = true;
    }
    if (stats.seasonYear != year) {
        if (stats.seasonYear) for (auto& c : stats.zone) c.season = 0;
        stats.seasonYear = year;
        dirty = true;
    }
}

// Reads straight into the (static) buffer – no copy on the stack (4 KB!).
// On error `out` is cleared again.
template <typename T>
static bool readFile(const char* path, T& out, uint32_t magic) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    bool ok = f.read(reinterpret_cast<uint8_t*>(&out), sizeof(T)) == sizeof(T) && out.magic == magic;
    f.close();
    if (!ok) memset(&out, 0, sizeof(T));
    return ok;
}

template <typename T>
static void writeFile(const char* path, const T& data) {
    File f = LittleFS.open(path, "w");
    if (!f) return;
    f.write(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
    f.close();
}

void begin() {
    memset(&logbuf, 0, sizeof(logbuf));
    memset(&stats, 0, sizeof(stats));
    if (!readFile(LOG_PATH, logbuf, LOG_MAGIC) || logbuf.head >= LOG_ENTRIES || logbuf.count > LOG_ENTRIES) {
        memset(&logbuf, 0, sizeof(logbuf));
    }
    readFile(STATS_PATH, stats, STATS_MAGIC);
    logbuf.magic = LOG_MAGIC;
    stats.magic = STATS_MAGIC;
    lastSave = millis();
    LOG("Log: %u entries loaded", logbuf.count);
}

void flush() {
    if (!dirty) return;
    dirty = false;
    lastSave = millis();
    writeFile(LOG_PATH, logbuf);
    writeFile(STATS_PATH, stats);
}

void loop() {
    static uint32_t lastRoll = 0;
    if (millis() - lastRoll >= 60000) {
        lastRoll = millis();
        rollCounters();
    }
    if (dirty && millis() - lastSave >= LOG_SAVE_MS) flush();
}

void add(Action a, uint8_t zone, uint8_t program, uint32_t durationS, uint8_t source, const char* reason) {
    Entry& e = logbuf.entries[logbuf.head];
    e = Entry{};
    e.t = ntp::valid() ? (uint32_t)ntp::now() : 0;
    e.duration = durationS;
    e.action = (uint8_t)a;
    e.zone = zone;
    e.program = program;
    e.source = source;
    e.reason = reasonCode(reason);
    logbuf.head = (logbuf.head + 1) % LOG_ENTRIES;
    if (logbuf.count < LOG_ENTRIES) logbuf.count++;
    dirty = true;
}

void addRuntime(uint8_t zone, uint32_t durationS) {
    if (zone >= ZONE_COUNT || !durationS) return;
    rollCounters();
    Counters& c = stats.zone[zone];
    c.total += durationS;
    c.week += durationS;
    c.season += durationS;
    dirty = true;
}

void runtimeJson(JsonObject o, uint8_t zone) {
    if (zone >= ZONE_COUNT) return;
    const Counters& c = stats.zone[zone];
    o["total_s"] = c.total;
    o["week_s"] = c.week;
    o["season_s"] = c.season;
}

size_t entryJson(uint16_t i, char* buf, size_t len) {
    if (i >= logbuf.count) return 0;
    const Entry& e = logbuf.entries[(logbuf.head + LOG_ENTRIES - 1 - i) % LOG_ENTRIES];

    char time[24] = "";
    if (e.t) ntp::format(e.t, time, sizeof(time), true);
    const char* action = e.action < sizeof(ACTIONS) / sizeof(ACTIONS[0]) ? ACTIONS[e.action] : "?";
    const char* source = e.source < sizeof(SOURCES) / sizeof(SOURCES[0]) ? SOURCES[e.source] : "?";

    // All values are fixed codes or numbers: no JSON escaping needed.
    int n = snprintf(buf, len, "{\"time\":%s%s%s,\"action\":\"%s\",\"source\":\"%s\"", e.t ? "\"" : "",
                     e.t ? time : "null", e.t ? "\"" : "", action, source);
    if (e.zone) n += snprintf(buf + n, len - n, ",\"zone\":%u", e.zone);
    if (e.program) n += snprintf(buf + n, len - n, ",\"program\":%u", e.program == PROGRAM_TEST ? 0 : e.program);
    if (e.duration) n += snprintf(buf + n, len - n, ",\"duration_s\":%lu", (unsigned long)e.duration);
    if (e.reason) n += snprintf(buf + n, len - n, ",\"reason\":\"%s\"", e.reason < REASON_COUNT ? REASONS[e.reason] : "other");
    n += snprintf(buf + n, len - n, "}");
    return n < (int)len ? n : 0;
}

}  // namespace history
