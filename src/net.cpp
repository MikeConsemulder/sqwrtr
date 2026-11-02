#include "net.h"
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <user_interface.h>  // rst_info
#include "config.h"
#include "secrets.h"
#include "events.h"
#include "ntp.h"
#include "programs.h"
#include "log.h"

namespace net {

static bool connected = false;
static bool ap = false;
static bool apManual = false;
static uint32_t apManualAt = 0;
static bool servicesStarted = false;
static uint32_t lostSince = 0;       // millis() when WiFi dropped (or boot)
static uint32_t connectedSince = 0;
static OtaHooks otaHooks;

static WiFiEventHandler gotIpHandler, disconnectHandler;

static void startAp(const char* reason) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    ap = true;
    LOG("WiFi: fallback AP '%s' active, IP %s", AP_SSID, WiFi.softAPIP().toString().c_str());
    events::emit("wifi_fallback_ap", "{\"reason\":\"%s\"}", reason);
}

static void stopAp() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    ap = false;
    apManual = false;
    LOG("WiFi: fallback AP off");
}

static void startServices() {
    // mDNS and OTA work on both STA and AP; starting once is enough.
    if (MDNS.begin(HOSTNAME)) {
        LOG("mDNS: %s.local", HOSTNAME);
    } else {
        LOG("mDNS: failed to start");
    }

    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        const char* what = ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem";
        LOG("OTA: start (%s)", what);
        if (otaHooks.onStart) otaHooks.onStart();
        events::emit("update_started", "{\"type\":\"%s\"}", what);
    });
    ArduinoOTA.onEnd([]() {
        events::emit("update_finished");
        LOG("OTA: done, restarting");
    });
    ArduinoOTA.onError([](ota_error_t e) { events::emit("update_failed", "{\"error\":%u}", e); });
    ArduinoOTA.begin();
    LOG("OTA: ready for updates");
    servicesStarted = true;
}

// ---------- reconnect ----------
// Every WIFI_RETRY_MS while offline: scan and connect to an access point of our SSID, strongest
// first; each attempt takes the next one of the best few (mesh: another node), and every
// WIFI_RESET_EVERY attempts the radio is switched off and on. No restart: the clock would be lost
// without NTP, and valves/programs don't depend on Wi-Fi anyway.

static uint16_t attempt = 0;
static bool scanning = false;
static uint32_t lastRetry = 0;

// Last resort: still offline after WIFI_RESTART_MS → restart, which reliably reconnects on mesh
// networks that keep refusing a running station. Only when idle and no program starts soon; the
// clock survives it (ntp::saveForRestart). At most WIFI_MAX_RESTARTS in a row, so the fallback AP
// still comes up when the network is really gone (e.g. changed password).
struct RtcNet {
    uint32_t magic;
    uint32_t restarts;
};
static const uint32_t RTC_NET_MAGIC = 0x4E455431;  // "NET1"
static uint8_t restarts = 0;

static void saveRestarts() {
    RtcNet r = {RTC_NET_MAGIC, restarts};
    ESP.rtcUserMemoryWrite(RTC_OFFSET_NET, reinterpret_cast<uint32_t*>(&r), sizeof(r));
}

static void loadRestarts() {
    RtcNet r;
    if (ESP.rtcUserMemoryRead(RTC_OFFSET_NET, reinterpret_cast<uint32_t*>(&r), sizeof(r)) &&
        r.magic == RTC_NET_MAGIC && r.restarts <= WIFI_MAX_RESTARTS &&
        ESP.getResetInfoPtr()->reason == REASON_SOFT_RESTART) {
        restarts = r.restarts;
    }
}

static bool restartSafe() {
    if (otaHooks.idle && !otaHooks.idle()) return false;
    if (ap && WiFi.softAPgetStationNum() > 0) return false;  // someone is using the fallback AP
    const time_t next = programs::nextRun();
    return !next || !ntp::valid() || next - ntp::now() > 180;  // no start within 3 min
}

static void maybeRestart(uint32_t now) {
    if (restarts >= WIFI_MAX_RESTARTS || now - lostSince < WIFI_RESTART_MS || !restartSafe()) return;
    restarts++;
    saveRestarts();
    LOG("WiFi: offline for %lu s, restarting to recover (%u/%u)", (unsigned long)(now - lostSince) / 1000,
        restarts, WIFI_MAX_RESTARTS);
    if (otaHooks.beforeRestart) otaHooks.beforeRestart();
    ntp::saveForRestart();
    Serial.flush();
    ESP.restart();
}

static void resetRadio() {
    LOG("WiFi: resetting radio");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    WiFi.mode(ap ? WIFI_AP_STA : WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    if (ap) WiFi.softAP(AP_SSID, AP_PASSWORD);
}

static void connectBest(int8_t found) {
    struct Candidate {
        int32_t rssi, channel;
        uint8_t bssid[6];
    };
    static const uint8_t MAX_CAND = 3;
    Candidate cand[MAX_CAND];
    uint8_t count = 0;
    for (int8_t i = 0; i < found; i++) {
        if (WiFi.SSID(i) != WIFI_SSID) continue;
        Candidate c{WiFi.RSSI(i), WiFi.channel(i), {}};
        memcpy(c.bssid, WiFi.BSSID(i), 6);
        // Insert sorted by signal, keep the best MAX_CAND.
        uint8_t pos = count;
        while (pos > 0 && cand[pos - 1].rssi < c.rssi) pos--;
        if (pos >= MAX_CAND) continue;
        for (uint8_t j = min<uint8_t>(count, MAX_CAND - 1); j > pos; j--) cand[j] = cand[j - 1];
        cand[pos] = c;
        if (count < MAX_CAND) count++;
    }
    WiFi.scanDelete();

    if (!count) {
        LOG("WiFi: '%s' not found, retrying (attempt %u)", WIFI_SSID, attempt);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        return;
    }
    const Candidate& c = cand[(attempt - 1) % count];
    const uint8_t* b = c.bssid;
    LOG("WiFi: attempt %u via %02x:%02x:%02x:%02x:%02x:%02x (channel %ld, %ld dBm)", attempt, b[0], b[1], b[2],
        b[3], b[4], b[5], (long)c.channel, (long)c.rssi);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, c.channel, c.bssid);
}

void begin(const OtaHooks& ota) {
    otaHooks = ota;

    WiFi.persistent(false);  // no flash writes on every connect
    WiFi.mode(WIFI_STA);
    // Modem sleep makes the ESP miss ARP requests (unreachable while associated), notably on mesh networks.
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.hostname(HOSTNAME);
    WiFi.setAutoReconnect(true);

    gotIpHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& e) {
        connected = true;
        connectedSince = millis();
        LOG("WiFi: connected, IP %s, RSSI %d dBm", e.ip.toString().c_str(), WiFi.RSSI());
    });
    disconnectHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& e) {
        static uint8_t lastReason = 0;
        static uint32_t lastLog = 0;
        if (connected) {
            lostSince = millis();
            LOG("WiFi: connection lost (reason %u)", e.reason);
        } else if (e.reason == WIFI_DISCONNECT_REASON_ASSOC_LEAVE) {
            // Our own disconnect before a new attempt: not interesting.
        } else if (e.reason != lastReason || millis() - lastLog >= 30000) {
            // Failed connect attempts: log the reason, but not on every retry.
            LOG("WiFi: connect failed (reason %u)", e.reason);
            lastLog = millis();
        }
        lastReason = e.reason;
        connected = false;
    });

    loadRestarts();
    lostSince = millis();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    LOG("WiFi: connecting to '%s'...", WIFI_SSID);
}

void startApManual() {
    apManual = true;
    apManualAt = millis();
    if (!ap) startAp("button");
}

void loop() {
    uint32_t now = millis();

    if (!servicesStarted && (connected || ap)) startServices();

    // The SDK's auto-reconnect gives up after some failures and keeps trying the same access point,
    // even when that mesh node refuses us (reason 202/203). So we retry ourselves.
    if (connected) {
        attempt = 0;
        if (restarts) {
            restarts = 0;
            saveRestarts();
        }
    } else if (scanning) {
        const int8_t r = WiFi.scanComplete();
        if (r != WIFI_SCAN_RUNNING || now - lastRetry >= 15000) {
            scanning = false;
            connectBest(r);
        }
    } else if (now - lostSince >= WIFI_RETRY_MS && now - lastRetry >= WIFI_RETRY_MS) {
        lastRetry = now;
        attempt++;
        if (attempt % WIFI_RESET_EVERY == 0) resetRadio();
        WiFi.disconnect(false);
        WiFi.scanNetworks(true);  // async; connectBest() picks a node when it's done
        scanning = true;
    }
    if (!connected) maybeRestart(now);

    if (!connected && !ap && now - lostSince >= WIFI_AP_FALLBACK_MS) startAp("no_wifi");
    if (connected && ap && now - connectedSince >= WIFI_AP_LINGER_MS &&
        (!apManual || now - apManualAt >= WIFI_AP_MANUAL_MS)) {
        stopAp();
    }

    if (servicesStarted) {
        MDNS.update();
        // Don't handle OTA invitations while valves are open: espota then gets
        // "No response" and nothing is flashed.
        if (!otaHooks.allowed || otaHooks.allowed()) ArduinoOTA.handle();
    }
}

bool wifiConnected() { return connected; }
bool apActive() { return ap; }

}  // namespace net
