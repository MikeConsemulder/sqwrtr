# sqwrtr – ESP8266 beregeningscontroller + Home Assistant

Zelfgebouwde beregeningscontroller voor een manifold met 5 magneetkleppen (24 V AC).
De ESP draait zelfstandig (eigen schema's, eigen webserver, REST-API) en wordt
geïntegreerd met Home Assistant via MQTT. Communicatie met de gebruiker: **Nederlands**.
Web-UI in het Nederlands.

---

## 1. Hardware (vastgesteld)

| Onderdeel | Details |
|---|---|
| Controller | DOIT ESP-12F DevKit V3 (NodeMCU-klasse, ESP8266, 4 MB flash, CH340G USB-serial) |
| Relais | 8-kanaals relaismodule, Songle SRD-05VDC-SL-C, optocouplers, **active LOW**, JD-VCC jumper |
| Voeding kleppen | AC/AC adapter EI66-24-1250: 230 V → **24 V AC, 30 W** (~1,25 A) |
| Voeding logica | LM2596 buck-module met bruggelijkrichter, gevoed uit dezelfde 24 V AC, uitgang **5,0 V** |
| Kleppen | 5× 24 V AC magneetklep (normaal gesloten) |

### Pinout

| ESP pin | GPIO | Functie |
|---|---|---|
| D1 | 5 | Zone 1 → IN1 |
| D2 | 4 | Zone 2 → IN2 |
| D5 | 14 | Zone 3 → IN3 |
| D6 | 12 | Zone 4 → IN4 |
| D7 | 13 | Zone 5 → IN5 |
| D0 | 16 | Gereserveerd: hoofdklep/pomp → IN6 (later) |
| RX | 3 | Gereserveerd: flowmeter (interrupt, later) — serieel loggen dan uit |
| A0 | ADC | Gereserveerd: regensensor (later) |
| D3 | 0 | Onboard FLASH-knop: lang indrukken = noodstop; heel lang = AP-modus |
| D4 | 2 | Onboard blauwe LED (active LOW): status |
| D8 | 15 | **NIET gebruiken** (moet LOW bij boot; relaisingang trekt hem hoog → boot faalt) |

Relais 7 en 8: reserve.

### Bedrading
- JD-VCC jumper **verwijderd**. Relais `VCC` ← ESP `3V3`; `JD-VCC` ← 5 V buck; `GND` (5 V-blok) ← buck GND.
- ESP `VIN` ← 5 V buck, `GND` ← buck GND.
- 24 V AC draad A → buck-ingang + COM relais 1–5 (doorgelust via Wago).
- Relais NO → klep 1–5. Gezamenlijke (common) klepdraden → 24 V AC draad B + andere buck-ingang.
- **Nooit** 24 V AC-draad verbinden met logica-GND (sluit bruggelijkrichter kort).
- Bij USB-flashen in de kast: 5 V liefst loskoppelen.

### Hardware-checks voor eerste inbedrijfname
1. Buck-uitgang op 5,0 V zetten **vóór** aansluiten ESP/relaisbord.
2. Onbelaste DC-spanning over de grote condensator meten: LM2596 max 40 V. Bij > 37 V: HV-variant overwegen.
3. Vermogensbudget: klep ~0,4 A inschakel / ~0,25 A houd → max 2 kleppen gelijktijdig.

---

## 2. Firmware – technische keuzes

- **PlatformIO**, Arduino-core ESP8266, board `nodemcuv2`, flash-layout 4M met ~1 MB LittleFS.
- Libraries (onderhouden varianten kiezen):
  - `ESP32Async/ESPAsyncWebServer` + `ESP32Async/ESPAsyncTCP` (webserver + WebSocket)
  - `bertmelis/espMqttClient` (MQTT, async)
  - `bblanchon/ArduinoJson` v7
  - `ArduinoOTA` (OTA vanuit PlatformIO)
- Config en schema's in **LittleFS** als JSON. Schrijven beperken (flash-slijtage).
- Tijd via NTP, tijdzone `CET-1CEST,M3.5.0,M10.5.0/3`.
- **`include/secrets.h`** (in `.gitignore`): WiFi SSID/wachtwoord (hardcoded, wijzigt voorlopig niet), OTA-wachtwoord, standaard admin-wachtwoord, API-key, MQTT-host/gebruiker/wachtwoord. Lever een `secrets.example.h`.
- mDNS: `sqwrtr.local`. DHCP (vast IP via DHCP-reservering in router).
- RAM is krap (~80 KB): web-UI als vanilla HTML/CSS/JS, gzip, geen frameworks. Geen grote JSON-buffers op de stack.
- Non-blocking code: geen `delay()` in de hoofdlus; alles via state machines/timers.

### Veilig opstarten (kritiek)
- Bij boot: eerst `digitalWrite(pin, HIGH)` en **dan** `pinMode(pin, OUTPUT)` → relais blijven uit (active LOW).
- Na (her)start: **alle kleppen dicht**, lopend programma wordt afgebroken, event `boot` met reset-reden.
- Hardware-watchdog actief; vastloper → reboot → alles dicht.

### Updates (OTA)
1. Eerste keer via USB.
2. Daarna `ArduinoOTA` vanuit PlatformIO (wachtwoord-beveiligd).
3. Uploadpagina `/update` in de web-UI (firmware `.bin` en LittleFS-image). Deze pagina staat **in de firmware zelf** (PROGMEM), niet in LittleFS, zodat herstel altijd mogelijk is.
4. Later: update via Home Assistant (MQTT `update`-entiteit; ESP haalt `.bin` op van URL die HA/GitHub aanlevert).
- ESP8266 heeft geen automatische rollback → OTA alleen toestaan als geen klep open is; na update self-test.

### Netwerk-robuustheid
- WiFi weg → blijven proberen, schema's lopen door op lokale tijd.
- 5 min geen WiFi → **fallback access point** (`sqwrtr-setup`, met wachtwoord) naast blijven proberen; web-UI daar ook bereikbaar.
- MQTT weg → reconnect met backoff; lokaal alles door laten werken.

---

## 3. Functionaliteit ESP

### Zones (5, aantal configureerbaar voor uitbreiding)
Per zone: naam, type (`gazon` / `border`, standaard `gazon`), ingeschakeld ja/nee,
standaardduur, max looptijd (standaard 120 min), looptijdtellers (totaal, per week, per seizoen).

### Handbediening
- Klep openen **voor bepaalde tijd** (minuten/seconden).
- Klep openen **voor onbepaalde tijd** → valt toch onder max looptijd (veiligheidslimiet).
- Klep sluiten, **noodstop** (alles dicht + lopende programma's afbreken).

### Veiligheidsregels
| Instelling | Standaard |
|---|---|
| Max gelijktijdig open | 2 (instelbaar) |
| Max looptijd per klep | 120 min (per zone instelbaar) |
| Vertraging tussen inschakelen kleppen | 500 ms |
| Pauze tussen zones in programma | instelbaar, standaard 5 s |

- Handmatige opdracht terwijl limiet bereikt → **geweigerd** (HTTP 409 + event `command_rejected`).
- Programmastappen die niet passen → **wachtrij**.
- Wintermodus → alle opdrachten en schema's geblokkeerd (behalve noodstop).

### Programma's (schema's draaien op de ESP)
- Max ~8 programma's. Per programma: naam, aan/uit, 1–4 starttijden, weekdagen óf elke N dagen, lijst stappen `{zone, duur}` (sequentieel).
- Overlappende programma's → wachtrij met inachtneming van max gelijktijdig.
- **Seizoensfactor** (0–200 %, globaal) en optioneel **factor per zonetype** → schaalt alle looptijden. Wordt door HA bijgestuurd.
- **Regenpauze / overslaan**: "sla X uur over" of "sla volgende run over", met reden (tekst) die HA meestuurt.
- Run-nu-knop per programma.

### Modi
`auto` (normaal), `handmatig-alleen` (schema's uit), `winter` (alles uit), `test` (testcyclus: elke ingeschakelde zone 1 min achter elkaar).

### Logboek
Ringbuffer (~200 entries) in RAM + periodiek naar LittleFS: tijd, zone/programma, actie, duur, bron (`web` / `api` / `mqtt` / `schema` / `knop`), reden.

### Status-LED
Uit = alles ok/rust, langzaam knipperen = geen WiFi, snel = AP-modus, aan = klep open.

---

## 4. REST-API

Basis `/api`, JSON. Auth: header `X-API-Key: <key>` (of `Authorization: Bearer <key>`). Web-UI: login met admin-wachtwoord → sessiecookie.

| Methode | Pad | Doel |
|---|---|---|
| GET | `/api/status` | Totale status: zones, actieve runs, resterende tijd, wachtrij, modus, regenpauze, factor |
| GET | `/api/info` | Firmwareversie, uptime, vrij heap, RSSI, IP, reset-reden |
| GET / PUT | `/api/zones`, `/api/zones/{id}` | Zones lezen/instellen |
| POST | `/api/zones/{id}/open` | body `{ "duration_s": 600 }` of `{}` voor onbepaald |
| POST | `/api/zones/{id}/close` | Sluiten |
| POST | `/api/stop` | Noodstop |
| GET / POST / PUT / DELETE | `/api/programs[/{id}]` | Programma's beheren |
| POST | `/api/programs/{id}/run` | Programma nu starten |
| POST | `/api/skip` | body `{ "hours": 24, "reason": "regen verwacht" }`; `{ "hours": 0 }` heft op |
| PUT | `/api/mode` | `auto` / `handmatig` / `winter` |
| POST | `/api/test` | Testcyclus |
| GET / PUT | `/api/settings` | Veiligheid, factor, MQTT, tijdzone, wachtwoorden, API-key |
| GET | `/api/log` | Logboek |
| POST | `/api/reboot` | Herstart |
| WS | `/ws` | Live events en status-push voor de web-UI |

Fouten: consistente JSON `{ "error": "code", "message": "..." }`.

---

## 5. Events (push via MQTT + WebSocket)

`zone_opened`, `zone_closed` (met werkelijke duur en reden), `program_started`, `program_finished`
(totale looptijd), `program_skipped` (reden), `command_rejected` (reden: limiet / winter / ...),
`max_runtime_reached`, `emergency_stop`, `queue_changed`, `mode_changed`, `boot` (reset-reden,
"kleppen waren open bij reboot" indien van toepassing), `wifi_fallback_ap`, `update_started`,
`update_finished`, `update_failed`.
Later: `leak_detected`, `no_flow` (flowmeter), `rain_detected` (regensensor).

---

## 6. MQTT + Home Assistant discovery

- Basis-topic: `sqwrtr/<device_id>/...` (`device_id` = configureerbaar, standaard `sqwrtr-<chipid>`) → klaar voor een 2e controller.
- Availability/LWT: `sqwrtr/<id>/status` = `online` / `offline`.
- State: `sqwrtr/<id>/zone/<n>/state` (`open` / `closed`), `.../zone/<n>/remaining`, `sqwrtr/<id>/state` (JSON totaal).
- Commando's: `sqwrtr/<id>/zone/<n>/set`, `.../cmd` (JSON: stop, skip, mode, run_program, test).
- Events: `sqwrtr/<id>/event` (JSON, niet retained).
- Discovery onder `homeassistant/...` met één device en entiteiten:
  - `valve` per zone (open/dicht)
  - `number` per zone (handmatige duur)
  - `sensor`: resterende tijd, actief programma, wachtrij, looptijdtellers, RSSI, uptime
  - `select` modus
  - `number` seizoensfactor
  - `button`: noodstop, testcyclus, programma's starten
  - `binary_sensor`: overslaan actief
  - `update`: firmware

---

## 7. Web-UI

- Mobiel-eerst, licht en snel, NL. Dashboard met 5 zonekaarten (status, resterende tijd, open-voor-X / open / sluiten), grote noodstopknop, actief programma + wachtrij, modus, regenpauze.
- Pagina's: Dashboard · Programma's (editor) · Zones · Logboek · Instellingen (veiligheid, MQTT, API-key genereren, wachtwoord, tijd) · Systeem (info, update-upload, herstart, backup/restore config als JSON).
- Live updates via WebSocket.

---

## 8. Home Assistant-kant

- **Mosquitto-add-on installeren** (draait nog niet) + MQTT-integratie.
- Rolverdeling: **ESP voert schema's uit, HA stuurt bij** (seizoensfactor + overslaan). Valt HA uit → beregening loopt door.
- **Weerlogica (eigen, Open-Meteo, gratis, geen key)** via REST-sensor:
  - Overslaan als verwachte neerslag komende 24 u > drempel (bijv. 3 mm) of gevallen afgelopen 24 u > drempel.
  - Seizoensfactor op basis van verdamping (ET₀) en temperatuur; aparte factor gazon vs borders/heg.
  - Drempels als `input_number` in HA, instelbaar.
- **Meldingen (HA companion app)**: fouten en veiligheid, ESP offline, start/einde programma, overgeslagen door weer (met reden), firmware-updates.
- **Onderhoud**: wintermodus, testcyclus, looptijdtellers per klep, firmware-update via HA.
- Lever als HA-**package** (YAML) + dashboard-YAML. Later eventueel eigen HACS-integratie.

---

## 9. Bouwvolgorde

1. **Fundament**: PlatformIO-project, secrets, veilige pin-init, WiFi, mDNS, ArduinoOTA, serieel loggen. Eerste flash via USB → daarna alleen nog draadloos.
2. **Klepbesturing + veiligheid**: zone-manager, limieten, max looptijd, stagger, noodstop, FLASH-knop, LED.
3. **REST-API + auth + `/update`-pagina** (herstelpad).
4. **Web-UI** (dashboard, zones, instellingen, systeem).
5. **Programma's**: NTP, scheduler, wachtrij, factor, overslaan, persistentie, logboek.
6. **MQTT + HA discovery + events.**
7. **HA-package**: weerlogica Open-Meteo, meldingen, dashboard, onderhoud.
8. **Later**: firmware-update via HA, HACS-integratie, flowmeter (RX), regensensor (A0), hoofdklep/pomp (D0), relais 7–8 / I/O-expander, 2e controller.

Elke fase: compileren, flashen, testen op hardware (zonder 24 V eerst met relais-LED's), dan pas verder.

## 10. Open punten
- Zonenamen en indeling gazon / borders: nog onbekend → instelbaar in UI, standaard `gazon`.
- Onbelaste DC-spanning buck-ingang nog te meten.
