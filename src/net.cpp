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
static uint32_t lostSince = 0;       // millis() toen WiFi wegviel (of boot)
static uint32_t connectedSince = 0;
static OtaHooks otaHooks;

static WiFiEventHandler gotIpHandler, disconnectHandler;

static void startAp(const char* reason) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    ap = true;
    LOG("WiFi: fallback-AP '%s' actief, IP %s", AP_SSID, WiFi.softAPIP().toString().c_str());
    events::emit("wifi_fallback_ap", "{\"reden\":\"%s\"}", reason);
}

static void stopAp() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    ap = false;
    apManual = false;
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
        const char* what = ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem";
        LOG("OTA: start (%s)", what);
        if (otaHooks.onStart) otaHooks.onStart();
        events::emit("update_started", "{\"type\":\"%s\"}", what);
    });
    ArduinoOTA.onEnd([]() {
        events::emit("update_finished");
        LOG("OTA: klaar, herstart");
    });
    ArduinoOTA.onError([](ota_error_t e) { events::emit("update_failed", "{\"fout\":%u}", e); });
    ArduinoOTA.begin();
    LOG("OTA: klaar voor updates");
    servicesStarted = true;
}

void begin(const OtaHooks& ota) {
    otaHooks = ota;

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

void startApManual() {
    apManual = true;
    apManualAt = millis();
    if (!ap) startAp("knop");
}

void loop() {
    uint32_t now = millis();

    if (!servicesStarted && (connected || ap)) startServices();

    if (!connected && !ap && now - lostSince >= WIFI_AP_FALLBACK_MS) startAp("geen_wifi");
    if (connected && ap && now - connectedSince >= WIFI_AP_LINGER_MS &&
        (!apManual || now - apManualAt >= WIFI_AP_MANUAL_MS)) {
        stopAp();
    }

    if (servicesStarted) {
        MDNS.update();
        // Met open kleppen OTA-uitnodigingen niet verwerken: espota krijgt dan
        // "No response" en er wordt niets geflasht.
        if (!otaHooks.allowed || otaHooks.allowed()) ArduinoOTA.handle();
    }
}

bool wifiConnected() { return connected; }
bool apActive() { return ap; }

}  // namespace net
