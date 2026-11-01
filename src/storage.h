#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "zones.h"

// Persistente configuratie in LittleFS (/config.json): zones, veiligheids-
// instellingen en (gehashte) inloggegevens. Wijzigingen worden niet direct
// geschreven maar na SAVE_DELAY_MS zonder nieuwe wijziging (flash-slijtage).
namespace storage {

void begin();  // LittleFS mounten en config laden (na zones::begin())
void loop();

// Aanroepen na elke wijziging van zones/instellingen/inlog.
void markDirty();
void flush();  // openstaande wijziging direct schrijven (vóór herstart/update)

// Gedeeltelijke update: alleen aanwezige velden worden overgenomen en gevalideerd.
// Bij een fout blijft `out` ongewijzigd en wijst *err naar een melding.
bool zoneFromJson(JsonObjectConst in, zones::Config& out, const char** err);
bool settingsFromJson(JsonObjectConst in, zones::Settings& out, const char** err);
void zoneToJson(JsonObject o, const zones::Config& c);

// Backup/restore van zones + instellingen (zonder inloggegevens).
void exportBackup(JsonDocument& doc);
bool importBackup(JsonObjectConst in, const char** err);

// Inloggegevens. Zolang niets gewijzigd is gelden de waarden uit secrets.h.
bool checkPassword(const char* password);
bool checkApiKey(const char* key);
void setPassword(const char* password);
String generateApiKey();  // nieuwe key; alleen de hash wordt bewaard
bool passwordIsDefault();
bool apiKeyIsDefault();

}  // namespace storage
