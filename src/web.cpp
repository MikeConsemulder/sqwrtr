#include "web.h"
#include <ESPAsyncWebServer.h>
#include <flash_hal.h>
#include "api.h"
#include "auth.h"
#include "secrets.h"
#include "zones.h"
#include "events.h"
#include "log.h"
#include "storage.h"
#include "history.h"
#include "programs.h"
#include "web_pages.h"
#include "web_ui.h"  // gegenereerd door tools/build_web.py

namespace web {

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

static const size_t MAX_BODY = 6144;  // restore van config.json (incl. 8 programma's)
static const uint8_t MAX_WS_CLIENTS = 4;
static const uint32_t WS_PUSH_MIN_MS = 250;      // status-pushes bundelen
static const uint32_t WS_HEARTBEAT_MS = 15000;   // ook zonder wijziging af en toe status

static bool statusDirty = false;
static uint32_t lastStatusPush = 0;
static uint32_t lastCleanup = 0;
static const uint32_t UPDATE_STALL_MS = 30000;  // upload afgebroken → Update vrijgeven

static bool restartPending = false;
static uint32_t restartAt = 0;

// Toestand van de lopende /update-upload (er kan er maar één tegelijk lopen).
static bool updating = false;
static uint32_t updateLastData = 0;
static int updateCode = 200;
static String updateError;

bool updateInProgress() { return updating; }

void scheduleRestart() {
    restartPending = true;
    restartAt = millis() + 500;
}

void markStatusDirty() { statusDirty = true; }

void pushEvent(const char* type, const char* detailJson) {
    statusDirty = true;
    if (!ws.count()) return;
    String msg;
    msg.reserve(48 + strlen(type) + strlen(detailJson));
    msg += F("{\"type\":\"event\",\"event\":\"");
    msg += type;
    msg += F("\",\"data\":");
    msg += detailJson;
    msg += '}';
    ws.textAll(msg);
}

static void pushStatus() {
    JsonDocument doc;
    doc["type"] = "status";
    JsonDocument status;
    api::buildStatus(status);
    doc["data"] = status;
    String msg;
    serializeJson(doc, msg);
    ws.textAll(msg);
}

// Body van API-requests verzamelen in req->_tempObject (wordt door de library vrijgegeven).
static void collectBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
    if (total > MAX_BODY) return;
    if (index == 0) {
        req->_tempObject = malloc(total + 1);
        if (!req->_tempObject) return;
    }
    if (!req->_tempObject) return;
    memcpy(static_cast<uint8_t*>(req->_tempObject) + index, data, len);
    if (index + len == total) static_cast<char*>(req->_tempObject)[total] = '\0';
}

// /update accepteert sessiecookie, API-key of HTTP Basic (admin:<admin-wachtwoord>),
// zodat het ook met curl werkt als de webpagina niet meewerkt.
static bool updateAuthorized(AsyncWebServerRequest* req) {
    return auth::check(req) != auth::Who::None || auth::basicOk(req);
}

static void failUpdate(int code, const String& error) {
    updateCode = code;
    updateError = error;
    if (updating) {
        Update.end();  // onvolledig → wordt niet geactiveerd
        updating = false;
        events::emit("update_failed", "{\"fout\":\"%s\"}", error.c_str());
    }
    LOG("Update: mislukt – %s", error.c_str());
}

static void onUpload(AsyncWebServerRequest* req, const String& filename, size_t index, uint8_t* data,
                     size_t len, bool final) {
    if (index == 0) {
        updateCode = 200;
        updateError = "";
        if (!updateAuthorized(req)) return failUpdate(401, "Niet ingelogd");
        if (updating) return failUpdate(409, "Er loopt al een update");
        if (zones::activeCount() > 0) return failUpdate(409, "Er staat een klep open; eerst sluiten");

        const bool fs = req->hasParam("type") && req->getParam("type")->value() == "fs";
        const size_t size = fs ? FS_PHYS_SIZE : ((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
        Update.runAsync(true);  // verplicht in async-context
        if (!Update.begin(size, fs ? U_FS : U_FLASH)) return failUpdate(500, Update.getErrorString());

        updating = true;
        storage::flush();
        history::flush();
        LOG("Update: start %s (%s)", fs ? "filesystem" : "firmware", filename.c_str());
        events::emit("update_started", "{\"type\":\"%s\",\"bron\":\"web\"}", fs ? "filesystem" : "firmware");
    }
    if (!updating) return;

    updateLastData = millis();
    if (Update.write(data, len) != len) return failUpdate(500, Update.getErrorString());
    if (final) {
        if (!Update.end(true)) return failUpdate(500, Update.getErrorString());
        updating = false;
        LOG("Update: klaar, %u bytes", (unsigned)(index + len));
        events::emit("update_finished", "{\"bytes\":%u}", (unsigned)(index + len));
    }
}

static void onUploadDone(AsyncWebServerRequest* req) {
    if (updateCode == 401 || !updateAuthorized(req)) {
        return req->send(401, "application/json", "{\"error\":\"unauthorized\",\"message\":\"Niet ingelogd\"}");
    }
    if (updateCode != 200 || updating) {
        String body = "{\"error\":\"update_failed\",\"message\":\"" + (updateError.length() ? updateError : String("Onvolledige upload")) + "\"}";
        return req->send(updateCode == 200 ? 500 : updateCode, "application/json", body);
    }
    req->send(200, "application/json", "{\"ok\":true,\"message\":\"Update geslaagd, herstarten\"}");
    scheduleRestart();
}

// Verstuurt een pagina die uit meerdere PROGMEM-blokken bestaat (zie web_pages.h).
template <size_t N>
static void sendPage(AsyncWebServerRequest* req, const char* const (&parts)[N]) {
    size_t total = 0;
    for (const char* p : parts) total += strlen_P(p);

    AsyncWebServerResponse* res = req->beginResponse(
        "text/html; charset=utf-8", total, [&parts](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
            size_t written = 0;
            size_t start = 0;  // begin van het huidige blok in de totale pagina
            for (const char* p : parts) {
                const size_t len = strlen_P(p);
                if (index < start + len) {
                    const size_t off = index - start;
                    const size_t n = std::min(len - off, maxLen - written);
                    memcpy_P(buf + written, p + off, n);
                    written += n;
                    index += n;
                    if (written == maxLen) break;
                }
                start += len;
            }
            return written;
        });
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

void begin() {
    // Zelfde auth als de API (sessiecookie wordt bij de handshake meegestuurd).
    ws.handleHandshake([](AsyncWebServerRequest* req) { return auth::check(req) != auth::Who::None; });
    ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType type, void*, uint8_t*, size_t) {
        if (type == WS_EVT_CONNECT) statusDirty = true;  // nieuwe client krijgt meteen de status
    });
    server.addHandler(&ws);

    server.on("/api", HTTP_ANY, api::handle, nullptr, collectBody);
    server.on(AsyncURIMatcher::exact("/update"), HTTP_GET, [](AsyncWebServerRequest* req) { sendPage(req, UPDATE_PAGE); });
    server.on(AsyncURIMatcher::exact("/update"), HTTP_POST, onUploadDone, onUpload);
    server.on(AsyncURIMatcher::exact("/"), HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* res = req->beginResponse(200, "text/html; charset=utf-8", WEB_UI_GZ, WEB_UI_GZ_LEN);
        res->addHeader("Content-Encoding", "gzip");
        res->addHeader("Cache-Control", "no-cache");
        req->send(res);
    });
    server.onNotFound([](AsyncWebServerRequest* req) { req->send(404, "text/plain", "Niet gevonden"); });
    server.begin();
    LOG("Web: server gestart op poort 80");
}

void loop() {
    const uint32_t now = millis();
    if (now - lastCleanup >= 1000) {
        lastCleanup = now;
        ws.cleanupClients(MAX_WS_CLIENTS);
    }
    if (ws.count() && now - lastStatusPush >= (statusDirty ? WS_PUSH_MIN_MS : WS_HEARTBEAT_MS)) {
        statusDirty = false;
        lastStatusPush = now;
        pushStatus();
    }

    if (updating && millis() - updateLastData > UPDATE_STALL_MS) failUpdate(500, "Upload afgebroken");
    if (restartPending && (int32_t)(millis() - restartAt) >= 0) {
        LOG("Herstarten...");
        programs::abortAll("herstart");
        zones::closeAll(zones::Source::System, "herstart");
        storage::flush();
        history::flush();
        ESP.restart();
    }
}

}  // namespace web
