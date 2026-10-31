// Kopieer naar include/secrets.h en vul in. secrets.h staat in .gitignore.
#pragma once

#define WIFI_SSID        "mijn-wifi"
#define WIFI_PASSWORD    "wifi-wachtwoord"

// Wachtwoord voor ArduinoOTA (zelfde waarde in env-var SQWRTR_OTA_PASS)
#define OTA_PASSWORD     "ota-wachtwoord"

// Fallback access point "sqwrtr-setup" (min. 8 tekens)
#define AP_PASSWORD      "setup-wachtwoord"

// Standaardwaarden; later wijzigbaar via web-UI
#define ADMIN_PASSWORD   "admin-wachtwoord"
#define API_KEY          "verander-mij"

#define MQTT_HOST        "homeassistant.local"
#define MQTT_PORT        1883
#define MQTT_USER        "sqwrtr"
#define MQTT_PASSWORD    "mqtt-wachtwoord"
