#include "ntp.h"
#include <coredecls.h>  // settimeofday_cb
#include <sys/time.h>   // settimeofday
#include <user_interface.h>  // rst_info
#include "config.h"
#include "log.h"

namespace ntp {

static char tz[48];
static bool synced = false;

struct RtcClock {
    uint32_t magic;
    uint32_t epoch;
};
static const uint32_t RTC_CLOCK_MAGIC = 0x434C4B31;  // "CLK1"

void saveForRestart() {
    if (!valid()) return;
    RtcClock c = {RTC_CLOCK_MAGIC, (uint32_t)time(nullptr)};
    ESP.rtcUserMemoryWrite(RTC_OFFSET_CLOCK, reinterpret_cast<uint32_t*>(&c), sizeof(c));
}

static void restoreAfterRestart() {
    RtcClock c;
    if (!ESP.rtcUserMemoryRead(RTC_OFFSET_CLOCK, reinterpret_cast<uint32_t*>(&c), sizeof(c))) return;
    const bool ok = c.magic == RTC_CLOCK_MAGIC && c.epoch > 1700000000;
    c.magic = 0;  // use once
    ESP.rtcUserMemoryWrite(RTC_OFFSET_CLOCK, reinterpret_cast<uint32_t*>(&c), sizeof(c));
    // Only after a software restart: after power loss RTC memory is garbage and the time unknown.
    if (!ok || ESP.getResetInfoPtr()->reason != REASON_SOFT_RESTART) return;
    timeval tv = {(time_t)c.epoch + 2, 0};  // + restart duration
    settimeofday(&tv, nullptr);
    LOG("Time: restored after restart (NTP will correct it)");
}

void begin() {
    if (!tz[0]) strlcpy(tz, DEFAULT_TZ, sizeof(tz));
    settimeofday_cb([](bool fromSntp) {
        if (!fromSntp) return;
        if (!synced) LOG("Time: synchronised via NTP");
        synced = true;
    });
    configTime(tz, NTP_SERVER_1, NTP_SERVER_2);
    restoreAfterRestart();
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
