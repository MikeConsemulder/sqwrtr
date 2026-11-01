#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Logboek (ringbuffer van LOG_ENTRIES in RAM, periodiek naar LittleFS) en
// looptijdtellers per zone (totaal / deze week / dit seizoen = kalenderjaar).
namespace history {

enum class Action : uint8_t {
    Boot, ZoneOpened, ZoneClosed, Rejected, MaxRuntime, EmergencyStop,
    ProgramStarted, ProgramFinished, ProgramSkipped, ModeChanged, SkipSet, Update,
};

void begin();  // na storage::begin() (LittleFS gemount)
void loop();
void flush();  // direct opslaan (vóór herstart/update)

static const uint8_t PROGRAM_TEST = 0xFF;  // testcyclus (program 0 = n.v.t.)

// zone en program 1-based, 0 = n.v.t. source = zones::Source als getal.
// reason: een van de bekende redencodes (zie history.cpp), anders "overig".
void add(Action a, uint8_t zone, uint8_t program, uint32_t durationS, uint8_t source, const char* reason);

// Zone (0-based) was durationS open → tellers bijwerken.
void addRuntime(uint8_t zone, uint32_t durationS);

void runtimeJson(JsonObject o, uint8_t zone);
// Logregel i (0 = nieuwste) als JSON-object in buf. 0 als die regel niet bestaat.
// Bewust zonder JsonDocument: /api/log streamt regel voor regel (200 regels als
// JsonDocument zou >20 KB heap kosten).
size_t entryJson(uint16_t i, char* buf, size_t len);

}  // namespace history
