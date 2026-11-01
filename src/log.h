#pragma once
#include <Arduino.h>

// Serial logging. Without the test console, Serial runs TX-only (RX free for the flow meter).
// Set LOG_ENABLED to 0 to disable logging entirely.
#ifndef LOG_ENABLED
#define LOG_ENABLED 1
#endif

#if LOG_ENABLED
#define LOG(fmt, ...) Serial.printf_P(PSTR("[%8lu] " fmt "\n"), millis(), ##__VA_ARGS__)
#else
#define LOG(fmt, ...) do {} while (0)
#endif
