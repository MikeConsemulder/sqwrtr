#include "api.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <memory>
#include <ESP8266WiFi.h>
#include "config.h"
#include "auth.h"
#include "zones.h"
#include "web.h"
#include "storage.h"
#include "programs.h"
#include "history.h"
#include "ntp.h"
#include "mqtt.h"
#include "log.h"

namespace api {

static uint32_t lastFailedLogin = 0;
static const uint32_t LOGIN_BACKOFF_MS = 2000;

// ---------- helpers ----------

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

// Reads the JSON body. Empty body → empty object. false on invalid JSON (error already sent).
static bool readBody(AsyncWebServerRequest* req, JsonDocument& doc) {
    const char* body = static_cast<const char*>(req->_tempObject);
    if (!body || !body[0]) {
        doc.to<JsonObject>();
        return true;
    }
    if (deserializeJson(doc, body) || !doc.is<JsonObject>()) {
        sendError(req, 400, "invalid_json", "Body is not a valid JSON object");
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

static void zoneJson(JsonObject o, uint8_t z, bool full) {
    const zones::Config& c = zones::config(z);
    o["id"] = z + 1;
    o["name"] = c.name;
    o["state"] = stateName(zones::state(z));
    o["remaining_s"] = zones::remainingS(z);
    o["pin"] = ZONE_PIN_LABELS[z];
    o["gpio"] = ZONE_PINS[z];
    o["relay"] = z + 1;  // relay input IN<n>
    o["enabled"] = c.enabled;
    o["default_duration_s"] = c.defaultDurationS;
    if (!full) return;
    storage::zoneToJson(o, c);
    history::runtimeJson(o["runtime"].to<JsonObject>(), z);
}

// Parse "3" → zone index 2; -1 if invalid.
static int parseZone(const String& s) {
    if (s.isEmpty() || s.length() > 3) return -1;
    for (char c : s) if (!isDigit(c)) return -1;
    int n = s.toInt();
    return (n >= 1 && n <= ZONE_COUNT) ? n - 1 : -1;
}

// ---------- endpoints ----------

void buildStatus(JsonDocument& doc) {
    programs::statusJson(doc.to<JsonObject>());  // mode, factor, rain delay, runs, queue, time
    JsonArray arr = doc["zones"].to<JsonArray>();
    for (uint8_t i = 0; i < ZONE_COUNT; i++) zoneJson(arr.add<JsonObject>(), i, false);
    doc["active"] = zones::activeCount();
    doc["max_concurrent"] = zones::settings().maxConcurrent;
    doc["uptime_s"] = millis() / 1000;
}

static void getStatus(AsyncWebServerRequest* req) {
    JsonDocument doc;
    buildStatus(doc);
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
    doc["mqtt"] = mqtt::stateName();
    sendJson(req, 200, doc);
}

static void putZone(AsyncWebServerRequest* req, uint8_t z) {
    JsonDocument body;
    if (!readBody(req, body)) return;
    const char* err = nullptr;
    if (!storage::zoneFromJson(body.as<JsonObjectConst>(), zones::config(z), &err)) {
        return sendError(req, 400, "invalid_field", err);
    }
    if (!zones::config(z).enabled) zones::close(z, zones::Source::Api, "disabled");
    storage::markDirty();
    web::markStatusDirty();

    JsonDocument doc;
    zoneJson(doc.to<JsonObject>(), z, true);
    sendJson(req, 200, doc);
}

static void openZone(AsyncWebServerRequest* req, uint8_t z, zones::Source src) {
    JsonDocument body;
    if (!readBody(req, body)) return;
    if (web::updateInProgress()) {
        return sendError(req, 409, "update_in_progress", "Firmware update in progress");
    }
    uint32_t duration = 0;  // 0 = indefinite
    if (!body["duration_s"].isNull()) {
        if (!body["duration_s"].is<uint32_t>() || body["duration_s"].as<uint32_t>() == 0) {
            return sendError(req, 400, "invalid_field", "duration_s: positive integer");
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
            snprintf(msg, sizeof(msg), "Max %u valves open at once", zones::settings().maxConcurrent);
            return sendError(req, 409, "limit_reached", msg);
        case zones::Result::Disabled:
            return sendError(req, 409, "zone_disabled", "Zone is disabled");
        case zones::Result::Locked:
            return sendError(req, 409, "winter", "Winter mode: everything blocked");
        default:
            return sendError(req, 404, "not_found", "Unknown zone");
    }
}

static void getSettings(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["max_concurrent"] = zones::settings().maxConcurrent;
    doc["stagger_ms"] = zones::settings().staggerMs;
    doc["zone_count"] = ZONE_COUNT;
    doc["timezone"] = ntp::timezone();
    programs::settingsToJson(doc.as<JsonObject>());
    doc["password_default"] = storage::passwordIsDefault();
    doc["api_key_default"] = storage::apiKeyIsDefault();
    mqtt::configToJson(doc["mqtt"].to<JsonObject>(), false);
    sendJson(req, 200, doc);
}

static void putSettings(AsyncWebServerRequest* req) {
    JsonDocument body;
    if (!readBody(req, body)) return;
    // Validate everything before applying
    const char* err = nullptr;
    JsonObjectConst in = body.as<JsonObjectConst>();
    zones::Settings zs = zones::settings();
    programs::Settings ps = programs::settings();
    if (!storage::settingsFromJson(in, zs, &err) || !programs::settingsFromJson(in, ps, &err)) {
        return sendError(req, 400, "invalid_field", err);
    }
    const char* tz = in["timezone"] | "";
    if (!in["timezone"].isNull() && (strlen(tz) < 3 || strlen(tz) > 47)) {
        return sendError(req, 400, "invalid_field", "timezone: POSIX time zone, 3-47 characters");
    }
    mqtt::Config mc = mqtt::config();
    if (!in["mqtt"].isNull()) {
        if (!in["mqtt"].is<JsonObjectConst>()) return sendError(req, 400, "invalid_field", "mqtt: object");
        if (!mqtt::configFromJson(in["mqtt"], mc, &err)) return sendError(req, 400, "invalid_field", err);
    }
    zones::settings() = zs;
    programs::settings() = ps;
    if (tz[0]) ntp::setTimezone(tz);
    if (!in["mqtt"].isNull()) {
        mqtt::config() = mc;
        mqtt::applyConfig();
    }
    storage::markDirty();
    web::markStatusDirty();
    getSettings(req);
}

static void changePassword(AsyncWebServerRequest* req) {
    JsonDocument body;
    if (!readBody(req, body)) return;
    if (!auth::passwordOk(body["current"] | "")) {
        return sendError(req, 403, "wrong_password", "Current password is incorrect");
    }
    const char* next = body["new"] | "";
    size_t len = strlen(next);
    if (len < 8 || len > 64) return sendError(req, 400, "invalid_field", "New password: 8-64 characters");
    storage::setPassword(next);
    auth::endAllSessions();  // log out everywhere, except here (new session)
    LOG("Web: admin password changed");
    AsyncResponseStream* res = req->beginResponseStream("application/json");
    res->addHeader("Set-Cookie", auth::createSession());
    res->print("{\"ok\":true}");
    req->send(res);
}

static void newApiKey(AsyncWebServerRequest* req) {
    JsonDocument doc;
    doc["api_key"] = storage::generateApiKey();
    LOG("Web: new API key generated");
    sendJson(req, 200, doc);
}

static void getBackup(AsyncWebServerRequest* req) {
    JsonDocument doc;
    storage::exportBackup(doc);
    AsyncResponseStream* res = req->beginResponseStream("application/json");
    res->addHeader("Content-Disposition", "attachment; filename=\"sqwrtr-config.json\"");
    serializeJsonPretty(doc, *res);
    req->send(res);
}

static void restore(AsyncWebServerRequest* req) {
    JsonDocument body;
    if (!readBody(req, body)) return;
    const char* err = nullptr;
    if (!storage::importBackup(body.as<JsonObjectConst>(), &err)) {
        return sendError(req, 400, "invalid_backup", err);
    }
    web::markStatusDirty();
    sendOk(req);
}

static void putProgram(AsyncWebServerRequest* req, uint8_t id, bool create) {
    JsonDocument body;
    if (!readBody(req, body)) return;
    if (create) {
        id = programs::freeId();
        if (!id) return sendError(req, 409, "full", "At most 8 programs");
    }
    programs::Program p;
    const char* err = nullptr;
    if (!programs::fromJson(body.as<JsonObjectConst>(), p, &err)) return sendError(req, 400, "invalid_field", err);
    programs::store(id, p);
    JsonDocument doc;
    programs::toJson(doc.to<JsonObject>(), id);
    sendJson(req, create ? 201 : 200, doc);
}

static void runResult(AsyncWebServerRequest* req, programs::RunResult r) {
    using programs::RunResult;
    if (r == RunResult::Ok) return sendOk(req);
    const int code = r == RunResult::NotFound ? 404 : 409;
    const char* error = r == RunResult::NotFound ? "not_found"
                      : r == RunResult::Empty ? "empty"
                      : r == RunResult::Winter ? "winter"
                      : r == RunResult::AlreadyRunning ? "already_running" : "queue_full";
    sendError(req, code, error, programs::runResultMessage(r));
}

static void skip(AsyncWebServerRequest* req, zones::Source src) {
    JsonDocument body;
    if (!readBody(req, body)) return;
    const char* reason = body["reason"] | "";
    if (body["next"] | false) {
        programs::setSkipNext(reason, src);
        return sendOk(req);
    }
    if (body["hours"].isNull()) return sendError(req, 400, "invalid_field", "hours (0 = clear) or next: true");
    int hours = body["hours"] | -1;
    if (hours < 0 || hours > 24 * 14) return sendError(req, 400, "invalid_field", "hours: 0-336");
    if (!programs::setSkipHours(hours, reason, src)) {
        return sendError(req, 409, "no_time", "Clock not set yet (no NTP)");
    }
    sendOk(req);
}

static void putMode(AsyncWebServerRequest* req, zones::Source src) {
    JsonDocument body;
    if (!readBody(req, body)) return;
    programs::Mode m;
    if (!programs::parseMode(body["mode"] | "", m)) {
        return sendError(req, 400, "invalid_field", "mode: auto, manual or winter");
    }
    programs::setMode(m, src);
    JsonDocument doc;
    doc["mode"] = programs::modeName();
    sendJson(req, 200, doc);
}

static void getLog(AsyncWebServerRequest* req) {
    uint16_t limit = 50;
    if (req->hasParam("limit")) limit = constrain(req->getParam("limit")->value().toInt(), 1, LOG_ENTRIES);
    // Stream entry by entry (chunked) instead of everything in one JsonDocument.
    struct State {
        uint16_t next = 0, limit;
        bool opened = false, closed = false;
        char pending[176];
        size_t pendingLen = 0, pendingPos = 0;
    };
    auto st = std::make_shared<State>();
    st->limit = limit;
    req->send(req->beginChunkedResponse("application/json", [st](uint8_t* buf, size_t maxLen, size_t) -> size_t {
        size_t n = 0;
        while (n < maxLen) {
            if (st->pendingPos < st->pendingLen) {  // rest of the previous entry
                size_t c = std::min(st->pendingLen - st->pendingPos, maxLen - n);
                memcpy(buf + n, st->pending + st->pendingPos, c);
                st->pendingPos += c;
                n += c;
                continue;
            }
            if (st->closed) break;
            char* p = st->pending;
            size_t len = 0;
            if (!st->opened) {
                st->opened = true;
                p[len++] = '[';
            }
            size_t e = st->next < st->limit ? history::entryJson(st->next, p + len + 1, sizeof(st->pending) - len - 1) : 0;
            if (e) {
                p[len] = st->next ? ',' : ' ';
                len += 1 + e;
                st->next++;
            } else {
                p[len++] = ']';
                st->closed = true;
            }
            st->pendingLen = len;
            st->pendingPos = 0;
        }
        return n;  // 0 = done
    }));
}

static void login(AsyncWebServerRequest* req) {
    JsonDocument body;
    if (!readBody(req, body)) return;
    if (lastFailedLogin && millis() - lastFailedLogin < LOGIN_BACKOFF_MS) {
        return sendError(req, 429, "too_many_attempts", "Wait a moment and try again");
    }
    if (!auth::passwordOk(body["password"] | "")) {
        lastFailedLogin = millis();
        LOG("Web: login failed from %s", req->client()->remoteIP().toString().c_str());
        return sendError(req, 401, "unauthorized", "Incorrect password");
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
    String path = req->url().substring(4);  // after "/api"
    if (path.endsWith("/")) path.remove(path.length() - 1);
    const WebRequestMethod m = req->method();

    if (path == "/login" && m == HTTP_POST) return login(req);

    const auth::Who who = auth::check(req);
    if (who == auth::Who::None) return sendError(req, 401, "unauthorized", "Login or API key required");
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
    if (path == "/settings/password" && m == HTTP_POST) return changePassword(req);
    if (path == "/settings/api-key" && m == HTTP_POST) return newApiKey(req);
    if (path == "/backup" && m == HTTP_GET) return getBackup(req);
    if (path == "/restore" && m == HTTP_POST) return restore(req);
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
        if (z < 0) return sendError(req, 404, "not_found", "Unknown zone");

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

    if (path == "/programs") {
        if (m == HTTP_GET) {
            JsonDocument doc;
            programs::listJson(doc.to<JsonArray>());
            return sendJson(req, 200, doc);
        }
        if (m == HTTP_POST) return putProgram(req, 0, true);
    }
    if (path.startsWith("/programs/")) {
        String rest = path.substring(10);
        int slash = rest.indexOf('/');
        String action = slash < 0 ? "" : rest.substring(slash + 1);
        String num = slash < 0 ? rest : rest.substring(0, slash);
        int id = num.toInt();
        if (num != String(id) || !programs::exists(id)) return sendError(req, 404, "not_found", "Unknown program");
        if (action == "" && m == HTTP_GET) {
            JsonDocument doc;
            programs::toJson(doc.to<JsonObject>(), id);
            return sendJson(req, 200, doc);
        }
        if (action == "" && m == HTTP_PUT) return putProgram(req, id, false);
        if (action == "" && m == HTTP_DELETE) {
            programs::remove(id);
            return sendOk(req);
        }
        if (action == "run" && m == HTTP_POST) return runResult(req, programs::runNow(id, src));
    }
    if (path == "/test" && m == HTTP_POST) return runResult(req, programs::runNow(0, src));
    if (path == "/skip" && m == HTTP_POST) return skip(req, src);
    if (path == "/mode" && m == HTTP_PUT) return putMode(req, src);
    if (path == "/log" && m == HTTP_GET) return getLog(req);

    sendError(req, 404, "not_found", "Unknown endpoint or method");
}

}  // namespace api
