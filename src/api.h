#pragma once

class AsyncWebServerRequest;

// REST-API onder /api (CLAUDE.md §4). Body (JSON) staat, als die er is,
// als nul-getermineerde string in req->_tempObject (zie web.cpp).
namespace api {

void handle(AsyncWebServerRequest* req);

}  // namespace api
