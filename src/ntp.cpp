#include "ntp.h"
#include <coredecls.h>  // settimeofday_cb
#include "config.h"
#include "log.h"

namespace ntp {

static char tz[48];
static bool synced = false;

void begin() {
    if (!tz[0]) strlcpy(tz, DEFAULT_TZ, sizeof(tz));
    settimeofday_cb([]() {
        if (!synced) LOG("Tijd: gesynchroniseerd via NTP");
        synced = true;
    });
    configTime(tz, NTP_SERVER_1, NTP_SERVER_2);
}

bool valid() { return synced || time(nullptr) > 1700000000; }

time_t now() { return time(nullptr); }

bool localNow(struct tm& out) {
    if (!valid()) return false;
    time_t t = time(nullptr);
    localtime_r(&t, &out);
    return true;
}

int32_t dayNumber(const struct tm& t) {
    // days_from_civil (Howard Hinnant)
    int y = t.tm_year + 1900;
    unsigned m = t.tm_mon + 1, d = t.tm_mday;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

void format(time_t t, char* buf, size_t len, bool seconds) {
    if (!t) {
        buf[0] = '\0';
        return;
    }
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(buf, len, seconds ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d %H:%M", &lt);
}

const char* timezone() { return tz[0] ? tz : DEFAULT_TZ; }

bool setTimezone(const char* value) {
    size_t n = strlen(value);
    if (n < 3 || n >= sizeof(tz)) return false;
    strlcpy(tz, value, sizeof(tz));
    setenv("TZ", tz, 1);
    tzset();
    return true;
}

}  // namespace ntp
