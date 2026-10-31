#include "net.h"
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include "config.h"
#include "secrets.h"
#include "log.h"

namespace net {

static bool connected = false;
static bool ap = false;
static bool servicesStarted = false;
static uint32_t lostSince = 0;       // millis() toen WiFi wegviel (of boot)
static uint32_t connectedSince = 0;
static std::function<void()> otaStartCb;

static WiFiEventHandler gotIpHandler, disconnectHandler;

static void startAp() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    ap = true;
    LOG("WiFi: fallback-AP '%s' actief, IP %s", AP_SSID, WiFi.softAPIP().toString().c_str());
    // TODO fase 6: event wifi_fallback_ap
}

static void stopAp() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    ap = false;
    LOG("WiFi: fallback-AP uit");
}

static void startServices() {
    // mDNS en OTA werken op STA én AP; één keer starten is genoeg.
    if (MDNS.begin(HOSTNAME)) {
        LOG("mDNS: %s.local", HOSTNAME);
    } else {
        LOG("mDNS: starten mislukt");
    }

    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        LOG("OTA: start (%s)", ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem");
        if (otaStartCb) otaStartCb();
    });
    ArduinoOTA.onEnd([]() { LOG("OTA: klaar, herstart"); });
    ArduinoOTA.onError([](ota_error_t e) { LOG("OTA: fout %u", e); });
    ArduinoOTA.begin();
    LOG("OTA: klaar voor updates");
    servicesStarted = true;
}

void begin(std::function<void()> onOtaStart) {
    otaStartCb = onOtaStart;

    WiFi.persistent(false);  // geen flash-writes bij elke connect
    WiFi.mode(WIFI_STA);
    WiFi.hostname(HOSTNAME);
    WiFi.setAutoReconnect(true);

    gotIpHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& e) {
        connected = true;
        connectedSince = millis();
        LOG("WiFi: verbonden, IP %s, RSSI %d dBm", e.ip.toString().c_str(), WiFi.RSSI());
    });
    disconnectHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& e) {
        if (connected) {
            lostSince = millis();
            LOG("WiFi: verbinding verbroken (reden %u)", e.reason);
        }
        connected = false;
    });

    lostSince = millis();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    LOG("WiFi: verbinden met '%s'...", WIFI_SSID);
}

void loop() {
    uint32_t now = millis();

    if (!servicesStarted && (connected || ap)) startServices();

    if (!connected && !ap && now - lostSince >= WIFI_AP_FALLBACK_MS) startAp();
    if (connected && ap && now - connectedSince >= WIFI_AP_LINGER_MS) stopAp();

    if (servicesStarted) {
        MDNS.update();
        ArduinoOTA.handle();
    }
}

bool wifiConnected() { return connected; }
bool apActive() { return ap; }

}  // namespace net
