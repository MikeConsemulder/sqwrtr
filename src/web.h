#pragma once

// HTTP-server op poort 80: REST-API (/api), testpagina (/) en het
// herstelpad /update. Alle pagina's staan in de firmware (PROGMEM), niet in
// LittleFS, zodat /update altijd werkt.
namespace web {

void begin();
void loop();

bool updateInProgress();

// Herstart na korte vertraging, zodat het HTTP-antwoord nog verstuurd wordt.
void scheduleRestart();

}  // namespace web
