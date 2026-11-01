#pragma once

// Serial test console (via `pio device monitor`). Uses RX (GPIO3):
// disable with -DCONSOLE_ENABLED=0 once the flow meter is connected to RX.
#ifndef CONSOLE_ENABLED
#define CONSOLE_ENABLED 1
#endif

namespace console {

void loop();

}  // namespace console
