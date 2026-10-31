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

// WebSocket /ws: event doorsturen naar verbonden clients (detail = JSON-object).
// Elk event markeert ook de status als gewijzigd → status-push volgt.
void pushEvent(const char* type, const char* detailJson);

// Status opnieuw pushen zonder event (bijv. na wijzigen van een zonenaam).
void markStatusDirty();

}  // namespace web
