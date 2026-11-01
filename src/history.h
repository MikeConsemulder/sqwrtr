#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Log (ring buffer of LOG_ENTRIES in RAM, periodically written to LittleFS) and
// runtime counters per zone (total / this week / this season = calendar year).
namespace history {

enum class Action : uint8_t {
    Boot, ZoneOpened, ZoneClosed, Rejected, MaxRuntime, EmergencyStop,
    ProgramStarted, ProgramFinished, ProgramSkipped, ModeChanged, SkipSet, Update,
};

void begin();  // after storage::begin() (LittleFS mounted)
void loop();
void flush();  // save immediately (before restart/update)

static const uint8_t PROGRAM_TEST = 0xFF;  // test cycle (program 0 = n/a)

// zone and program are 1-based, 0 = n/a. source = zones::Source as a number.
// reason: one of the known reason codes (see history.cpp), otherwise "other".
void add(Action a, uint8_t zone, uint8_t program, uint32_t durationS, uint8_t source, const char* reason);

// Zone (0-based) was open for durationS → update the counters.
void addRuntime(uint8_t zone, uint32_t durationS);

void runtimeJson(JsonObject o, uint8_t zone);
// Log entry i (0 = newest) as a JSON object in buf. 0 if that entry doesn't exist.
// Deliberately without JsonDocument: /api/log streams entry by entry (200 entries
// as a JsonDocument would cost >20 KB of heap).
size_t entryJson(uint16_t i, char* buf, size_t len);

}  // namespace history
