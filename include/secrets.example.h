// Copy to include/secrets.h and fill in. secrets.h is in .gitignore.
#pragma once

#define WIFI_SSID        "my-wifi"
#define WIFI_PASSWORD    "wifi-password"

// Password for ArduinoOTA (same value as the SQWRTR_OTA_PASS env var)
#define OTA_PASSWORD     "ota-password"

// Fallback access point "sqwrtr-setup" (min. 8 characters)
#define AP_PASSWORD      "setup-password"

// Defaults; can be changed later via the web UI
#define ADMIN_PASSWORD   "admin-password"
#define API_KEY          "change-me"

#define MQTT_HOST        "homeassistant.local"
#define MQTT_PORT        1883
#define MQTT_USER        "sqwrtr"
#define MQTT_PASSWORD    "mqtt-password"
