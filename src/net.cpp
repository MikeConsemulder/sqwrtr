#include "net.h"
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include "config.h"
#include "secrets.h"
#include "events.h"
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

void begin(const OtaHooks& ota) {
    otaHooks = ota;

    WiFi.persistent(false);  // no flash writes on every connect
    WiFi.mode(WIFI_STA);
    WiFi.hostname(HOSTNAME);
    WiFi.setAutoReconnect(true);

    gotIpHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& e) {
        connected = true;
        connectedSince = millis();
        LOG("WiFi: connected, IP %s, RSSI %d dBm", e.ip.toString().c_str(), WiFi.RSSI());
    });
    disconnectHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& e) {
        if (connected) {
            lostSince = millis();
            LOG("WiFi: connection lost (reason %u)", e.reason);
        }
        connected = false;
    });

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
