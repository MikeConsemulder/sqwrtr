#pragma once
#include <ArduinoJson.h>

class AsyncWebServerRequest;

// REST API under /api (CLAUDE.md §4). The body (JSON), if any, is available
// as a null-terminated string in req->_tempObject (see web.cpp).
namespace api {

void handle(AsyncWebServerRequest* req);

// Contents of GET /api/status; also used for the WebSocket push.
void buildStatus(JsonDocument& doc);

}  // namespace api
