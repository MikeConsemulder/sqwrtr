#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"
#include "zones.h"

// Programma's (schema's op de ESP), runs + wachtrij, modi, seizoensfactor en
// regenpauze. Een run is één programma dat zijn stappen na elkaar afwerkt;
// er lopen maximaal maxConcurrent runs tegelijk, de rest staat in de wachtrij.
namespace programs {

enum class Mode : uint8_t { Auto, Manual, Winter };

struct Step {
    uint8_t zone;        // 0-based
    uint16_t durationS;  // basisduur, vóór factoren
};

static const uint16_t NO_START = 0xFFFF;

struct Program {
    bool used;
    bool enabled;
    char name[24];
    uint16_t starts[MAX_STARTS];  // minuten na middernacht, NO_START = leeg
    uint8_t days;                 // bit 0 = maandag … bit 6 = zondag
    uint8_t everyDays;            // > 0: elke N dagen vanaf anchorDay (days genegeerd)
    int32_t anchorDay;            // lokaal dagnummer (ntp::dayNumber)
    uint8_t stepCount;
    Step steps[MAX_STEPS];
};

struct Settings {
    uint8_t seasonFactor;  // %, 0–200
    uint8_t factorGazon;   // %, 0–200
    uint8_t factorBorder;  // %, 0–200
    uint16_t zonePauseS;   // pauze tussen stappen
};

enum class RunResult : uint8_t { Ok, NotFound, Empty, Winter, AlreadyRunning, Full };

void begin();  // na zones::begin(); vóór storage::begin()
void loop();

// ---- modi ----
Mode mode();
void setMode(Mode m, zones::Source src);
const char* modeName();  // "auto" / "handmatig" / "winter" / "test" (testcyclus loopt)
bool parseMode(const char* s, Mode& out);

// ---- runs ----
RunResult runNow(uint8_t id, zones::Source src);  // id 1-based; 0 = testcyclus
void abortAll(const char* reason);
const char* runResultMessage(RunResult r);

// ---- regenpauze / overslaan ----
bool setSkipHours(uint32_t hours, const char* reason, zones::Source src);  // 0 = opheffen; false zonder klok
void setSkipNext(const char* reason, zones::Source src);
bool skipActive();

// ---- programma's beheren (id 1-based) ----
bool exists(uint8_t id);
int8_t freeId();  // 0 als vol
bool fromJson(JsonObjectConst in, Program& out, const char** err);
void store(uint8_t id, const Program& p);
void remove(uint8_t id);
void toJson(JsonObject o, uint8_t id);
void listJson(JsonArray arr);

// ---- instellingen + status ----
Settings& settings();
bool settingsFromJson(JsonObjectConst in, Settings& out, const char** err);
void settingsToJson(JsonObject o);
void statusJson(JsonObject o);  // modus, factor, regenpauze, runs, wachtrij, volgende run, tijd

// ---- opslag (via storage) ----
void exportConfig(JsonObject root);
bool importConfig(JsonObjectConst root, const char** err);  // eerst valideren, dan toepassen

}  // namespace programs
