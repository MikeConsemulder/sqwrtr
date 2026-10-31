#pragma once
#include <Arduino.h>

namespace events {

// Publiceer een event (CLAUDE.md §5). detailFmt bouwt een JSON-object, bijv.
//   events::emit("zone_opened", "{\"zone\":%u}", 1);
// Nu alleen serieel gelogd; fase 6 stuurt dit door naar MQTT en WebSocket.
void emit(const char* type, const char* detailFmt = nullptr, ...)
    __attribute__((format(printf, 2, 3)));

}  // namespace events
