#pragma once
#include <Arduino.h>

namespace relays {

// Zet alle relaispinnen veilig uit: eerst HIGH schrijven, dan pas OUTPUT.
// Mag meerdere keren aangeroepen worden (ook vanuit preinit()).
void safeInit();

// Alle kleppen (en hoofdklep) direct dicht.
void allOff();

}  // namespace relays
