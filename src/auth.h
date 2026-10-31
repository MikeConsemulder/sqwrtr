#pragma once
#include <Arduino.h>

class AsyncWebServerRequest;

// Toegang tot API en webpagina's:
//   - API-key in header `X-API-Key: <key>` of `Authorization: Bearer <key>` (HA, scripts)
//   - sessiecookie na inloggen met admin-wachtwoord (web-UI)
// Sessies staan alleen in RAM: na een herstart opnieuw inloggen.
namespace auth {

enum class Who : uint8_t { None, ApiKey, Session };

Who check(AsyncWebServerRequest* req);

bool passwordOk(const char* password);

// Maakt een nieuwe sessie; geeft de Set-Cookie-waarde terug.
String createSession();
void endSession(AsyncWebServerRequest* req);

static const char COOKIE_NAME[] = "sqwrtr_session";

}  // namespace auth
