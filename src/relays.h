#pragma once
#include <Arduino.h>

namespace relays {

// Zet alle relaispinnen veilig uit: eerst HIGH schrijven, dan pas OUTPUT.
// Mag meerdere keren aangeroepen worden (ook vanuit preinit()).
void safeInit();

// Leest uit RTC-geheugen welke kleppen open stonden vóór de reboot en wist dat.
// Aanroepen in setup(), na safeInit().
void begin();

// Bitmasker (bit 0 = zone 1) van kleppen die open stonden vóór de reboot.
uint8_t previousMask();

// Zone-relais (0-based) aan/uit. Alleen via de zone-manager gebruiken.
void setZone(uint8_t zone, bool on);

// Bitmasker van de kleppen die nu open staan.
uint8_t mask();

// Alle kleppen (en hoofdklep) direct dicht.
void allOff();

}  // namespace relays
