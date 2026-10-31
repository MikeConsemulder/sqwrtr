#pragma once
#include <ArduinoJson.h>

class AsyncWebServerRequest;

// REST-API onder /api (CLAUDE.md §4). Body (JSON) staat, als die er is,
// als nul-getermineerde string in req->_tempObject (zie web.cpp).
namespace api {

void handle(AsyncWebServerRequest* req);

// Inhoud van GET /api/status; ook gebruikt voor de WebSocket-push.
void buildStatus(JsonDocument& doc);

}  // namespace api
