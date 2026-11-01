#pragma once
#include <Arduino.h>
#include <functional>

// Zone manager: the only place that opens/closes valves and enforces the safety rules
// (max concurrent, max runtime, stagger, emergency stop). Zones are 0-based here;
// externally (events, console, API) always 1-based.
namespace zones {

enum class Type : uint8_t { Lawn, Border };
enum class State : uint8_t { Closed, Pending, Open };  // Pending = waiting for stagger
enum class Source : uint8_t { Web, Api, Mqtt, Schedule, Button, Console, System };
enum class Result : uint8_t { Ok, InvalidZone, Disabled, LimitReached, Locked };  // Locked = winter mode

struct Config {
    char name[24];
    Type type;
    bool enabled;
    uint32_t defaultDurationS;
    uint32_t maxRuntimeS;
};

struct Settings {
    uint8_t maxConcurrent;
    uint16_t staggerMs;
};

void begin();
void loop();

// durationS == 0 → indefinite, but always capped by maxRuntimeS.
// If the zone is already open, its runtime is adjusted (never beyond maxRuntimeS
// counted from the moment it opened).
Result open(uint8_t zone, uint32_t durationS, Source src);
bool close(uint8_t zone, Source src, const char* reason = "manual");
void closeAll(Source src, const char* reason);
void emergencyStop(Source src);

// Winter mode: open() refuses everything (closing and emergency stop keep working).
void setLocked(bool locked);
// Called after an emergency stop (abort programs).
void setStopHook(std::function<void()> hook);

State state(uint8_t zone);
uint32_t remainingS(uint8_t zone);
uint8_t activeCount();  // open + pending
bool anyOpen();

Config& config(uint8_t zone);
Settings& settings();

const char* sourceName(Source s);
const char* resultName(Result r);

}  // namespace zones
