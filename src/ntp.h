#pragma once
#include <Arduino.h>
#include <time.h>

// Time via NTP with a POSIX time zone. Schedules run on local time and keep
// running without WiFi once the clock has been set.
namespace ntp {

void begin();

bool valid();  // clock set (after the first NTP sync)
time_t now();
bool localNow(struct tm& out);  // false while the clock is not set

// Local day number (days since 1970-01-01, local date).
int32_t dayNumber(const struct tm& t);

// "2026-11-01 06:00" (local); "" if t == 0.
void format(time_t t, char* buf, size_t len, bool seconds = false);

const char* timezone();
bool setTimezone(const char* tz);  // false on invalid length

}  // namespace ntp
