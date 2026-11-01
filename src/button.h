#pragma once

// Onboard FLASH button (GPIO0):
//   held ≥ 2 s  → emergency stop
//   held ≥ 10 s → fallback AP on
namespace button {

void begin();
void loop();

}  // namespace button
