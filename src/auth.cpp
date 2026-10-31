#include "auth.h"
#include <ESPAsyncWebServer.h>
#include <libb64/cdecode.h>
#include "storage.h"

namespace auth {

static const uint8_t MAX_SESSIONS = 4;
static const uint8_t TOKEN_LEN = 32;  // hex-tekens
static const uint32_t SESSION_TTL_MS = 7UL * 24 * 3600 * 1000;

struct Session {
    char token[TOKEN_LEN + 1];
    uint32_t lastUsed;
};
static Session sessions[MAX_SESSIONS];

// Vergelijking zonder vroegtijdig stoppen (geen timing-lek).
static bool safeEquals(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    uint8_t diff = la != lb;
    for (size_t i = 0; i < la && i < lb; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

static Session* findSession(const char* token) {
    if (strlen(token) != TOKEN_LEN) return nullptr;
    const uint32_t now = millis();
    for (Session& s : sessions) {
        if (!s.token[0]) continue;
        if (now - s.lastUsed > SESSION_TTL_MS) {
            s.token[0] = '\0';
            continue;
        }
        if (safeEquals(s.token, token)) return &s;
    }
    return nullptr;
}

// Zoekt de waarde van onze cookie in de Cookie-header.
static bool cookieToken(AsyncWebServerRequest* req, char* out, size_t outLen) {
    if (!req->hasHeader("Cookie")) return false;
    const String& cookies = req->header("Cookie");
    String key = String(COOKIE_NAME) + "=";
    int start = cookies.indexOf(key);
    if (start < 0) return false;
    start += key.length();
    int end = cookies.indexOf(';', start);
    String value = cookies.substring(start, end < 0 ? cookies.length() : end);
    value.trim();
    strlcpy(out, value.c_str(), outLen);
    return true;
}

Who check(AsyncWebServerRequest* req) {
    if (req->hasHeader("X-API-Key") && storage::checkApiKey(req->header("X-API-Key").c_str())) {
        return Who::ApiKey;
    }
    if (req->hasHeader("Authorization")) {
        const String& h = req->header("Authorization");
        if (h.startsWith("Bearer ") && storage::checkApiKey(h.c_str() + 7)) return Who::ApiKey;
    }
    char token[TOKEN_LEN + 2];
    if (cookieToken(req, token, sizeof(token))) {
        Session* s = findSession(token);
        if (s) {
            s->lastUsed = millis();
            return Who::Session;
        }
    }
    return Who::None;
}

bool passwordOk(const char* password) { return storage::checkPassword(password); }

bool basicOk(AsyncWebServerRequest* req) {
    if (!req->hasHeader("Authorization")) return false;
    const String& h = req->header("Authorization");
    if (!h.startsWith("Basic ") || h.length() > 200) return false;
    char decoded[160];
    int n = base64_decode_chars(h.c_str() + 6, h.length() - 6, decoded);
    if (n <= 0 || n >= (int)sizeof(decoded)) return false;
    decoded[n] = '\0';
    char* colon = strchr(decoded, ':');
    if (!colon) return false;
    *colon = '\0';
    return !strcmp(decoded, "admin") && storage::checkPassword(colon + 1);
}

void endAllSessions() {
    for (Session& s : sessions) s.token[0] = '\0';
}

String createSession() {
    // Vrije plek, anders de oudste overschrijven.
    Session* slot = &sessions[0];
    for (Session& s : sessions) {
        if (!s.token[0]) { slot = &s; break; }
        if (s.lastUsed < slot->lastUsed) slot = &s;
    }
    for (uint8_t i = 0; i < TOKEN_LEN; i += 8) {
        snprintf(slot->token + i, 9, "%08x", (unsigned)ESP.random());  // hardware-RNG
    }
    slot->lastUsed = millis();
    return String(COOKIE_NAME) + "=" + slot->token + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=604800";
}

void endSession(AsyncWebServerRequest* req) {
    char token[TOKEN_LEN + 2];
    if (!cookieToken(req, token, sizeof(token))) return;
    Session* s = findSession(token);
    if (s) s->token[0] = '\0';
}

}  // namespace auth
