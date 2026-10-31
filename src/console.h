#pragma once

// Serieel testconsole (via `pio device monitor`). Gebruikt RX (GPIO3):
// uitzetten met -DCONSOLE_ENABLED=0 zodra de flowmeter op RX komt.
#ifndef CONSOLE_ENABLED
#define CONSOLE_ENABLED 1
#endif

namespace console {

void loop();

}  // namespace console
