#pragma once
#include <Arduino.h>

namespace relays {

// Puts all relay pins safely off: write HIGH first, only then set OUTPUT.
// May be called multiple times (also from preinit()).
void safeInit();

// Reads from RTC memory which valves were open before the reboot, then clears it.
// Call in setup(), after safeInit().
void begin();

// Bitmask (bit 0 = zone 1) of valves that were open before the reboot.
uint8_t previousMask();

// Zone relay (0-based) on/off. Only use via the zone manager.
void setZone(uint8_t zone, bool on);

// Bitmask of the valves that are open right now.
uint8_t mask();

// Close all valves (and the master valve) immediately.
void allOff();

}  // namespace relays
