#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "zones.h"

// Persistent configuration in LittleFS (/config.json): zones, safety
// settings and (hashed) credentials. Changes are not written immediately but
// after SAVE_DELAY_MS without a further change (flash wear).
namespace storage {

void begin();  // mount LittleFS and load the config (after zones::begin())
void loop();

// Call after every change to zones/settings/credentials.
void markDirty();
void flush();  // write a pending change immediately (before restart/update)

// Partial update: only fields that are present are taken over and validated.
// On error `out` is left unchanged and *err points to a message.
bool zoneFromJson(JsonObjectConst in, zones::Config& out, const char** err);
bool settingsFromJson(JsonObjectConst in, zones::Settings& out, const char** err);
void zoneToJson(JsonObject o, const zones::Config& c);

// Backup/restore of zones + settings (without credentials).
void exportBackup(JsonDocument& doc);
bool importBackup(JsonObjectConst in, const char** err);

// Credentials. Until changed, the values from secrets.h apply.
bool checkPassword(const char* password);
bool checkApiKey(const char* key);
void setPassword(const char* password);
String generateApiKey();  // new key; only the hash is stored
bool passwordIsDefault();
bool apiKeyIsDefault();

}  // namespace storage
