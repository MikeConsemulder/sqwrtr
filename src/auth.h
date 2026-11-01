#pragma once
#include <Arduino.h>

class AsyncWebServerRequest;

// Access to the API and web pages:
//   - API key in header `X-API-Key: <key>` or `Authorization: Bearer <key>` (HA, scripts)
//   - session cookie after logging in with the admin password (web UI)
// Sessions live in RAM only: log in again after a restart.
namespace auth {

enum class Who : uint8_t { None, ApiKey, Session };

Who check(AsyncWebServerRequest* req);

bool passwordOk(const char* password);

// HTTP Basic with user "admin" and the admin password (for curl to /update).
bool basicOk(AsyncWebServerRequest* req);

// Creates a new session; returns the Set-Cookie value.
String createSession();
void endSession(AsyncWebServerRequest* req);
void endAllSessions();  // after a password change

static const char COOKIE_NAME[] = "sqwrtr_session";

}  // namespace auth
