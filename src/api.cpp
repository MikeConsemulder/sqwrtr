#include "api.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include "config.h"
#include "auth.h"
#include "zones.h"
#include "web.h"
#include "log.h"

namespace api {

static uint32_t lastFailedLogin = 0;
static const uint32_t LOGIN_BACKOFF_MS = 2000;

// ---------- hulpfuncties ----------

static void sendJson(AsyncWebServerRequest* req, int code, const JsonDocument& doc) {
    AsyncResponseStream* res = req->beginResponseStream("application/json");
    res->setCode(code);
    serializeJson(doc, *res);
    req->send(res);
}

static void sendError(AsyncWebServerRequest* req, int code, const char* error, const char* message) {
    JsonDocument doc;
    doc["error"] = error;
    doc["message"] = message;
    sendJson(req, code, doc);
}

static void sendOk(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["ok"] = true;
    sendJson(req, 200, doc);
}

// Leest de JSON-body. Lege body → leeg object. false bij ongeldige JSON (fout al verstuurd).
static bool readBody(AsyncWebServerRequest* req, JsonDocument& doc) {
    const char* body = static_cast<const char*>(req->_tempObject);
    if (!body || !body[0]) {
        doc.to<JsonObject>();
        return true;
    }
    if (deserializeJson(doc, body) || !doc.is<JsonObject>()) {
        sendError(req, 400, "invalid_json", "Body is geen geldig JSON-object");
        return false;
    }
    return true;
}

static const char* stateName(zones::State s) {
    switch (s) {
        case zones::State::Open: return "open";
        case zones::State::Pending: return "pending";
        default: return "closed";
    }
}

static const char* typeName(zones::Type t) { return t == zones::Type::Border ? "border" : "gazon"; }

static void zoneJson(JsonObject o, uint8_t z, bool full) {
    const zones::Config& c = zones::config(z);
    o["id"] = z + 1;
    o["name"] = c.name;
    o["state"] = stateName(zones::state(z));
    o["remaining_s"] = zones::remainingS(z);
    if (!full) return;
    o["type"] = typeName(c.type);
    o["enabled"] = c.enabled;
    o["default_duration_s"] = c.defaultDurationS;
    o["max_runtime_s"] = c.maxRuntimeS;
}

// Parse "3" → zone-index 2; -1 als ongeldig.
static int parseZone(const String& s) {
    if (s.isEmpty() || s.length() > 3) return -1;
    for (char c : s) if (!isDigit(c)) return -1;
    int n = s.toInt();
    return (n >= 1 && n <= ZONE_COUNT) ? n - 1 : -1;
}

// ---------- endpoints ----------

static void getStatus(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["mode"] = "auto";  // TODO fase 5: modi
    JsonArray arr = doc["zones"].to<JsonArray>();
    for (uint8_t i = 0; i < ZONE_COUNT; i++) zoneJson(arr.add<JsonObject>(), i, false);
    doc["active"] = zones::activeCount();
    doc["max_concurrent"] = zones::settings().maxConcurrent;
    doc["queue"].to<JsonArray>();  // TODO fase 5
    doc["skip"] = nullptr;         // TODO fase 5
    doc["factor"] = 100;           // TODO fase 5
    doc["uptime_s"] = millis() / 1000;
    sendJson(req, 200, doc);
}

static void getInfo(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["version"] = FW_VERSION;
    doc["uptime_s"] = millis() / 1000;
    doc["heap_free"] = ESP.getFreeHeap();
    doc["heap_frag"] = ESP.getHeapFragmentation();
    doc["rssi"] = WiFi.RSSI();
    doc["ip"] = WiFi.localIP().toString();
    doc["hostname"] = HOSTNAME;
    doc["reset_reason"] = ESP.getResetReason();
    char chip[9];
    snprintf(chip, sizeof(chip), "%06x", ESP.getChipId());
    doc["chip_id"] = chip;
    sendJson(req, 200, doc);
}

static void putZone(AsyncWebServerRequest* req, uint8_t z) {
    JsonDocument body;
    if (!readBody(req, body)) return;
    zones::Config next = zones::config(z);

    if (body["name"].is<const char*>()) {
        const char* n = body["name"];
        if (!n[0] || strlen(n) >= sizeof(next.name)) {
            return sendError(req, 400, "invalid_field", "name: 1-23 tekens");
        }
        strlcpy(next.name, n, sizeof(next.name));
    }
    if (body["type"].is<const char*>()) {
        const char* t = body["type"];
        if (!strcmp(t, "gazon")) next.type = zones::Type::Gazon;
        else if (!strcmp(t, "border")) next.type = zones::Type::Border;
        else return sendError(req, 400, "invalid_field", "type: gazon of border");
    }
    if (body["enabled"].is<bool>()) next.enabled = body["enabled"];
    if (!body["max_runtime_s"].isNull()) {
        uint32_t v = body["max_runtime_s"] | 0u;
        if (v < 1 || v > 4 * 3600) return sendError(req, 400, "invalid_field", "max_runtime_s: 1-14400");
        next.maxRuntimeS = v;
    }
    if (!body["default_duration_s"].isNull()) {
        uint32_t v = body["default_duration_s"] | 0u;
        if (v < 1) return sendError(req, 400, "invalid_field", "default_duration_s: minimaal 1");
        next.defaultDurationS = v;
    }
    if (next.defaultDurationS > next.maxRuntimeS) {
        return sendError(req, 400, "invalid_field", "default_duration_s mag niet groter zijn dan max_runtime_s");
    }

    zones::config(z) = next;  // TODO fase 5: opslaan in LittleFS
    if (!next.enabled) zones::close(z, zones::Source::Api, "uitgeschakeld");

    JsonDocument doc;
    zoneJson(doc.to<JsonObject>(), z, true);
    sendJson(req, 200, doc);
}

static void openZone(AsyncWebServerRequest* req, uint8_t z, zones::Source src) {
    JsonDocument body;
    if (!readBody(req, body)) return;
    if (web::updateInProgress()) {
        return sendError(req, 409, "update_in_progress", "Firmware-update bezig");
    }
    uint32_t duration = 0;  // 0 = onbepaald
    if (!body["duration_s"].isNull()) {
        if (!body["duration_s"].is<uint32_t>() || body["duration_s"].as<uint32_t>() == 0) {
            return sendError(req, 400, "invalid_field", "duration_s: positief geheel getal");
        }
        duration = body["duration_s"];
    }

    char msg[64];
    switch (zones::open(z, duration, src)) {
        case zones::Result::Ok: {
            JsonDocument doc;
            zoneJson(doc.to<JsonObject>(), z, false);
            return sendJson(req, 200, doc);
        }
        case zones::Result::LimitReached:
            snprintf(msg, sizeof(msg), "Max %u kleppen tegelijk open", zones::settings().maxConcurrent);
            return sendError(req, 409, "limit_reached", msg);
        case zones::Result::Disabled:
            return sendError(req, 409, "zone_disabled", "Zone is uitgeschakeld");
        default:
            return sendError(req, 404, "not_found", "Onbekende zone");
    }
}

static void getSettings(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["max_concurrent"] = zones::settings().maxConcurrent;
    doc["stagger_ms"] = zones::settings().staggerMs;
    doc["zone_count"] = ZONE_COUNT;
    sendJson(req, 200, doc);
}

static void putSettings(AsyncWebServerRequest* req) {
    JsonDocument body;
    if (!readBody(req, body)) return;
    zones::Settings next = zones::settings();
    if (!body["max_concurrent"].isNull()) {
        uint32_t v = body["max_concurrent"] | 0u;
        if (v < 1 || v > ZONE_COUNT) return sendError(req, 400, "invalid_field", "max_concurrent: 1-5");
        next.maxConcurrent = v;
    }
    if (!body["stagger_ms"].isNull()) {
        uint32_t v = body["stagger_ms"] | 0xFFFFFFFFu;
        if (v > 10000) return sendError(req, 400, "invalid_field", "stagger_ms: 0-10000");
        next.staggerMs = v;
    }
    zones::settings() = next;  // TODO fase 5: opslaan in LittleFS
    getSettings(req);
}

static void login(AsyncWebServerRequest* req) {
    JsonDocument body;
    if (!readBody(req, body)) return;
    if (lastFailedLogin && millis() - lastFailedLogin < LOGIN_BACKOFF_MS) {
        return sendError(req, 429, "too_many_attempts", "Even wachten en opnieuw proberen");
    }
    if (!auth::passwordOk(body["password"] | "")) {
        lastFailedLogin = millis();
        LOG("Web: inloggen mislukt vanaf %s", req->client()->remoteIP().toString().c_str());
        return sendError(req, 401, "unauthorized", "Onjuist wachtwoord");
    }
    lastFailedLogin = 0;
    AsyncResponseStream* res = req->beginResponseStream("application/json");
    res->addHeader("Set-Cookie", auth::createSession());
    res->print("{\"ok\":true}");
    req->send(res);
}

static void logout(AsyncWebServerRequest* req) {
    auth::endSession(req);
    AsyncResponseStream* res = req->beginResponseStream("application/json");
    res->addHeader("Set-Cookie", String(auth::COOKIE_NAME) + "=; Path=/; Max-Age=0");
    res->print("{\"ok\":true}");
    req->send(res);
}

// ---------- router ----------

void handle(AsyncWebServerRequest* req) {
    String path = req->url().substring(4);  // na "/api"
    if (path.endsWith("/")) path.remove(path.length() - 1);
    const WebRequestMethod m = req->method();

    if (path == "/login" && m == HTTP_POST) return login(req);

    const auth::Who who = auth::check(req);
    if (who == auth::Who::None) return sendError(req, 401, "unauthorized", "Inloggen of API-key vereist");
    const zones::Source src = who == auth::Who::ApiKey ? zones::Source::Api : zones::Source::Web;

    if (path == "/logout" && m == HTTP_POST) return logout(req);
    if (path == "/status" && m == HTTP_GET) return getStatus(req);
    if (path == "/info" && m == HTTP_GET) return getInfo(req);
    if (path == "/stop" && m == HTTP_POST) {
        zones::emergencyStop(src);
        return sendOk(req);
    }
    if (path == "/settings") {
        if (m == HTTP_GET) return getSettings(req);
        if (m == HTTP_PUT) return putSettings(req);
    }
    if (path == "/reboot" && m == HTTP_POST) {
        web::scheduleRestart();
        return sendOk(req);
    }

    if (path == "/zones" && m == HTTP_GET) {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (uint8_t i = 0; i < ZONE_COUNT; i++) zoneJson(arr.add<JsonObject>(), i, true);
        return sendJson(req, 200, doc);
    }
    if (path.startsWith("/zones/")) {
        String rest = path.substring(7);
        int slash = rest.indexOf('/');
        String action = slash < 0 ? "" : rest.substring(slash + 1);
        int z = parseZone(slash < 0 ? rest : rest.substring(0, slash));
        if (z < 0) return sendError(req, 404, "not_found", "Onbekende zone");

        if (action == "" && m == HTTP_GET) {
            JsonDocument doc;
            zoneJson(doc.to<JsonObject>(), z, true);
            return sendJson(req, 200, doc);
        }
        if (action == "" && m == HTTP_PUT) return putZone(req, z);
        if (action == "open" && m == HTTP_POST) return openZone(req, z, src);
        if (action == "close" && m == HTTP_POST) {
            zones::close(z, src);
            JsonDocument doc;
            zoneJson(doc.to<JsonObject>(), z, false);
            return sendJson(req, 200, doc);
        }
    }

    // Komen in fase 5
    if (path.startsWith("/programs") || path == "/skip" || path == "/mode" || path == "/test" ||
        path == "/log") {
        return sendError(req, 501, "not_implemented", "Nog niet beschikbaar (fase 5)");
    }

    sendError(req, 404, "not_found", "Onbekend endpoint of methode");
}

}  // namespace api
