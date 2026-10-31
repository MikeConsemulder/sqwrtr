#pragma once
#include <Arduino.h>

namespace events {

// Publiceer een event (CLAUDE.md §5). detailFmt bouwt een JSON-object, bijv.
//   events::emit("zone_opened", "{\"zone\":%u}", 1);
// Gaat naar de seriële log en de WebSocket; fase 6 stuurt het ook naar MQTT.
void emit(const char* type, const char* detailFmt = nullptr, ...)
    __attribute__((format(printf, 2, 3)));

}  // namespace events
