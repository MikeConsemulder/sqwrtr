#pragma once
#include <Arduino.h>

// Serieel loggen (TX-only, zodat RX vrij blijft voor de flowmeter).
// Zet LOG_ENABLED op 0 om loggen helemaal uit te schakelen.
#ifndef LOG_ENABLED
#define LOG_ENABLED 1
#endif

#if LOG_ENABLED
#define LOG(fmt, ...) Serial.printf_P(PSTR("[%8lu] " fmt "\n"), millis(), ##__VA_ARGS__)
#else
#define LOG(fmt, ...) do {} while (0)
#endif
