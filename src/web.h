#pragma once

// HTTP server on port 80: REST API (/api), web UI (/) and the
// /update recovery path. All pages live in the firmware (PROGMEM), not in
// LittleFS, so /update always works.
namespace web {

void begin();
void loop();

bool updateInProgress();

// Restart after a short delay, so the HTTP response still gets sent.
void scheduleRestart();

// WebSocket /ws: forward an event to connected clients (detail = JSON object).
// Every event also marks the status as changed → a status push follows.
void pushEvent(const char* type, const char* detailJson);

// Push the status again without an event (e.g. after renaming a zone).
void markStatusDirty();

}  // namespace web
