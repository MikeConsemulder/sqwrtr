#pragma once
#include <Arduino.h>

// ---- Pinout (zie CLAUDE.md §1) ----
// Relaisingangen zijn active LOW: HIGH = relais uit = klep dicht.
static const uint8_t ZONE_PINS[] = {
    5,   // D1 – zone 1 → IN1
    4,   // D2 – zone 2 → IN2
    14,  // D5 – zone 3 → IN3
    12,  // D6 – zone 4 → IN4
    13,  // D7 – zone 5 → IN5
};
static const uint8_t ZONE_COUNT = sizeof(ZONE_PINS) / sizeof(ZONE_PINS[0]);

static const uint8_t PIN_MASTER_VALVE = 16;  // D0 – hoofdklep/pomp → IN6 (later)
static const uint8_t PIN_BUTTON       = 0;   // D3 – onboard FLASH-knop (active LOW)
static const uint8_t PIN_LED          = 2;   // D4 – onboard blauwe LED (active LOW)
// GPIO3 (RX) gereserveerd voor flowmeter → Serial draait TX-only.
// GPIO15 (D8) NIET gebruiken: moet LOW zijn bij boot.

// ---- Netwerk ----
static const char HOSTNAME[]         = "sqwrtr";
static const char AP_SSID[]          = "sqwrtr-setup";
static const uint32_t WIFI_AP_FALLBACK_MS = 5UL * 60 * 1000;  // 5 min zonder WiFi → AP
static const uint32_t WIFI_AP_LINGER_MS   = 60UL * 1000;      // AP nog 1 min aan na herverbinden
static const uint32_t WIFI_AP_MANUAL_MS   = 15UL * 60 * 1000; // AP via knop: min. 15 min aan

// ---- Veiligheid (standaardwaarden, later instelbaar via UI/API) ----
static const uint8_t  DEFAULT_MAX_CONCURRENT = 2;
static const uint16_t DEFAULT_STAGGER_MS     = 500;       // vertraging tussen inschakelen kleppen
static const uint32_t DEFAULT_MAX_RUNTIME_S  = 120 * 60;  // max looptijd per klep
static const uint32_t DEFAULT_DURATION_S     = 10 * 60;   // standaardduur per zone

// ---- FLASH-knop ----
static const uint32_t BUTTON_DEBOUNCE_MS = 50;
static const uint32_t BUTTON_ESTOP_MS    = 2000;   // lang indrukken → noodstop
static const uint32_t BUTTON_AP_MS       = 10000;  // heel lang → AP-modus

// ---- RTC-geheugen ----
// Blok 0–31 gebruikt de bootloader (eboot) voor OTA; wij zitten ruim daarna.
static const uint32_t RTC_OFFSET_RELAYS = 64;
