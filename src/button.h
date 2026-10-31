#pragma once

// Onboard FLASH-knop (GPIO0):
//   ≥ 2 s ingedrukt  → noodstop
//   ≥ 10 s ingedrukt → fallback-AP aan
namespace button {

void begin();
void loop();

}  // namespace button
