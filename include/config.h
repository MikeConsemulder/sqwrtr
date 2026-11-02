#pragma once
#include <Arduino.h>

// ---- Pinout (see CLAUDE.md §1) ----
// Relay inputs are active LOW: HIGH = relay off = valve closed.
static const uint8_t ZONE_PINS[] = {
    5,   // D1 – zone 1 → IN1
    4,   // D2 – zone 2 → IN2
    14,  // D5 – zone 3 → IN3
    12,  // D6 – zone 4 → IN4
    13,  // D7 – zone 5 → IN5
};
static const uint8_t ZONE_COUNT = sizeof(ZONE_PINS) / sizeof(ZONE_PINS[0]);
// Silkscreen labels on the ESP board, for the web UI (zone n → relay input IN<n>).
static const char* const ZONE_PIN_LABELS[] = {"D1", "D2", "D5", "D6", "D7"};
static_assert(sizeof(ZONE_PIN_LABELS) / sizeof(ZONE_PIN_LABELS[0]) == ZONE_COUNT, "one pin label per zone");

static const uint8_t PIN_MASTER_VALVE = 16;  // D0 – master valve/pump → IN6 (later)
static const uint8_t PIN_BUTTON       = 0;   // D3 – onboard FLASH button (active LOW)
static const uint8_t PIN_LED          = 2;   // D4 – onboard blue LED (active LOW)
// GPIO3 (RX) reserved for the flow meter → Serial runs TX-only.
// Do NOT use GPIO15 (D8): it must be LOW at boot.

// ---- Network ----
static const char HOSTNAME[]         = "sqwrtr";
static const char AP_SSID[]          = "sqwrtr-setup";
static const uint32_t WIFI_AP_FALLBACK_MS = 5UL * 60 * 1000;  // 5 min without WiFi → AP
static const uint32_t WIFI_RETRY_MS = 20000;                    // restart connect attempt while offline
static const uint32_t WIFI_AP_LINGER_MS   = 60UL * 1000;      // keep AP up 1 more min after reconnecting
static const uint32_t WIFI_AP_MANUAL_MS   = 15UL * 60 * 1000; // AP via button: at least 15 min

// ---- Safety (defaults, later configurable via UI/API) ----
static const uint8_t  DEFAULT_MAX_CONCURRENT = 2;
static const uint16_t DEFAULT_STAGGER_MS     = 500;       // delay between switching valves on
static const uint32_t DEFAULT_MAX_RUNTIME_S  = 120 * 60;  // max runtime per valve
static const uint32_t DEFAULT_DURATION_S     = 10 * 60;   // default duration per zone

// ---- Programs ----
static const uint8_t  MAX_PROGRAMS        = 8;
static const uint8_t  MAX_STEPS           = 10;   // steps per program
static const uint8_t  MAX_STARTS          = 4;    // start times per program
static const uint8_t  MAX_RUNS            = 8;    // running + queued runs
static const uint16_t DEFAULT_ZONE_PAUSE_S = 5;   // pause between zones in a program
static const uint16_t TEST_STEP_S         = 60;   // test cycle: 1 min per zone

// ---- Time ----
static const char DEFAULT_TZ[]   = "CET-1CEST,M3.5.0,M10.5.0/3";
static const char NTP_SERVER_1[] = "nl.pool.ntp.org";
static const char NTP_SERVER_2[] = "pool.ntp.org";

// ---- Log ----
static const uint8_t  LOG_ENTRIES     = 200;
static const uint32_t LOG_SAVE_MS     = 10UL * 60 * 1000;  // write to flash at most every 10 min

// ---- FLASH button ----
static const uint32_t BUTTON_DEBOUNCE_MS = 50;
static const uint32_t BUTTON_ESTOP_MS    = 2000;   // long press → emergency stop
static const uint32_t BUTTON_AP_MS       = 10000;  // very long press → AP mode

// ---- RTC memory ----
// Blocks 0–31 are used by the bootloader (eboot) for OTA; we stay well clear of them.
static const uint32_t RTC_OFFSET_RELAYS = 64;
