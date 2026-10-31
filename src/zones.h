#pragma once
#include <Arduino.h>

// Zone-manager: enige plek die kleppen opent/sluit en de veiligheidsregels bewaakt
// (max gelijktijdig, max looptijd, stagger, noodstop). Zones zijn hier 0-based;
// naar buiten (events, console, API) altijd 1-based.
namespace zones {

enum class Type : uint8_t { Gazon, Border };
enum class State : uint8_t { Closed, Pending, Open };  // Pending = wacht op stagger
enum class Source : uint8_t { Web, Api, Mqtt, Schema, Knop, Console, System };
enum class Result : uint8_t { Ok, InvalidZone, Disabled, LimitReached };

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

// durationS == 0 → onbepaald, maar altijd begrensd door maxRuntimeS.
// Staat de zone al open, dan wordt de looptijd aangepast (nooit voorbij maxRuntimeS
// gerekend vanaf het moment van openen).
Result open(uint8_t zone, uint32_t durationS, Source src);
bool close(uint8_t zone, Source src, const char* reason = "handmatig");
void closeAll(Source src, const char* reason);
void emergencyStop(Source src);

State state(uint8_t zone);
uint32_t remainingS(uint8_t zone);
uint8_t activeCount();  // open + wachtend
bool anyOpen();

Config& config(uint8_t zone);
Settings& settings();

const char* sourceName(Source s);
const char* resultName(Result r);

}  // namespace zones
