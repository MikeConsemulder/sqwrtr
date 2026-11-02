#pragma once
#include <Arduino.h>

namespace events {

// Publish an event (CLAUDE.md §5). detailFmt builds a JSON object, e.g.
//   events::emit("zone_opened", "{\"zone\":%u}", 1);
// Goes to the serial log, the WebSocket and MQTT (sqwrtr/<id>/event).
void emit(const char* type, const char* detailFmt = nullptr, ...)
    __attribute__((format(printf, 2, 3)));

}  // namespace events
