#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// MQTT client + Home Assistant discovery (CLAUDE.md §6). Everything keeps working
// locally without a broker; the client reconnects with backoff.
// Base topic: sqwrtr/<device_id>/... ; discovery under <prefix>/<component>/<device_id>/...
namespace mqtt {

struct Config {
    bool enabled;
    char host[64];
    uint16_t port;
    char user[32];
    char password[64];
    char deviceId[24];  // empty = sqwrtr-<chipid>; also the MQTT client id (max 23)
    char prefix[24];    // discovery prefix, default "homeassistant"
};

void begin();  // before storage::begin() (sets the defaults from secrets.h)
void loop();

Config& config();
// Partial update: only present fields; validated. "password": null/absent = keep.
bool configFromJson(JsonObjectConst in, Config& out, const char** err);
void configToJson(JsonObject o, bool withPassword);
void applyConfig();  // reconnect with the current config (after a change)

// Status for the UI: "disabled", "connected", "connecting" or "disconnected".
const char* stateName();
const char* lastError();

// Called by events::emit (any context): forwarded to sqwrtr/<id>/event.
void publishEvent(const char* type, const char* detailJson);
// Something in the status changed (mode, factor, runs, ...): republish sqwrtr/<id>/state.
void markStateDirty();

}  // namespace mqtt
