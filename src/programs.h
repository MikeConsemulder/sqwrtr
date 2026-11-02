#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"
#include "zones.h"

// Programs (schedules on the ESP), runs + queue, modes, season factor and
// rain delay. A run is one program working through its steps one after another;
// at most maxConcurrent runs are active at once, the rest wait in the queue.
namespace programs {

enum class Mode : uint8_t { Auto, Manual, Winter };

struct Step {
    uint8_t zone;        // 0-based
    uint16_t durationS;  // base duration, before factors
};

static const uint16_t NO_START = 0xFFFF;

struct Program {
    bool used;
    bool enabled;
    char name[24];
    uint16_t starts[MAX_STARTS];  // minutes after midnight, NO_START = empty
    uint8_t days;                 // bit 0 = Monday … bit 6 = Sunday
    uint8_t everyDays;            // > 0: every N days from anchorDay (days ignored)
    int32_t anchorDay;            // local day number (ntp::dayNumber)
    uint8_t stepCount;
    Step steps[MAX_STEPS];
};

struct Settings {
    uint8_t seasonFactor;  // %, 0–200
    uint8_t factorLawn;    // %, 0–200
    uint8_t factorBorder;  // %, 0–200
    uint16_t zonePauseS;   // pause between steps
};

enum class RunResult : uint8_t { Ok, NotFound, Empty, Winter, AlreadyRunning, Full };

void begin();  // after zones::begin(); before storage::begin()
void loop();

// ---- modes ----
Mode mode();
void setMode(Mode m, zones::Source src);
const char* modeName();  // "auto" / "manual" / "winter" / "test" (test cycle running)
bool parseMode(const char* s, Mode& out);

// ---- runs ----
RunResult runNow(uint8_t id, zones::Source src);  // id 1-based; 0 = test cycle
void abortAll(const char* reason);
bool busy();  // a run is active or queued
time_t nextRun(uint8_t* id = nullptr);  // next scheduled start (0 = none)
const char* runResultMessage(RunResult r);

// ---- rain delay / skip ----
bool setSkipHours(uint32_t hours, const char* reason, zones::Source src);  // 0 = clear; false without clock
void setSkipNext(const char* reason, zones::Source src);
bool skipActive();

// ---- program management (id 1-based) ----
bool exists(uint8_t id);
const char* name(uint8_t id);  // nullptr if the program doesn't exist
int8_t freeId();  // 0 if full
bool fromJson(JsonObjectConst in, Program& out, const char** err);
void store(uint8_t id, const Program& p);
void remove(uint8_t id);
void toJson(JsonObject o, uint8_t id);
void listJson(JsonArray arr);

// ---- settings + status ----
Settings& settings();
bool settingsFromJson(JsonObjectConst in, Settings& out, const char** err);
void settingsToJson(JsonObject o);
void statusJson(JsonObject o);  // mode, factor, rain delay, runs, queue, next run, time

// ---- persistence (via storage) ----
void exportConfig(JsonObject root);
bool importConfig(JsonObjectConst root, const char** err);  // validate first, then apply

}  // namespace programs
