#pragma once
#include <Arduino.h>
#include <time.h>

// Tijd via NTP met POSIX-tijdzone. Schema's draaien op lokale tijd en lopen
// door zonder WiFi zolang de klok eenmaal gezet is.
namespace ntp {

void begin();

bool valid();  // klok gezet (na eerste NTP-sync)
time_t now();
bool localNow(struct tm& out);  // false zolang de klok niet gezet is

// Lokaal dagnummer (dagen sinds 1970-01-01, lokale datum).
int32_t dayNumber(const struct tm& t);

// "2026-11-01 06:00" (lokaal); "" als t == 0.
void format(time_t t, char* buf, size_t len, bool seconds = false);

const char* timezone();
bool setTimezone(const char* tz);  // false bij ongeldige lengte

}  // namespace ntp
