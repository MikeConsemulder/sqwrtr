# sqwrtr – ESP8266 irrigation controller + Home Assistant

Self-built irrigation controller for a manifold with 5 solenoid valves (24 V AC).
The ESP runs stand-alone (own schedules, own web server, REST API) and integrates
with Home Assistant via MQTT.

**Language:** the repository is English (code, comments, UI, logs, API, docs).
Communicate with the maintainer in **Dutch**.

User-facing documentation lives in `README.md`; this file is the design spec and the
notes an AI assistant (or a new contributor) needs to work on the code.

---

## 1. Hardware (fixed)

| Part | Details |
|---|---|
| Controller | DOIT ESP-12F DevKit V3 (NodeMCU class, ESP8266, 4 MB flash, CH340G USB-serial) |
| Relays | 8-channel relay module, Songle SRD-05VDC-SL-C, optocouplers, **active LOW**, JD-VCC jumper |
| Valve power | AC/AC adapter EI66-24-1250: 230 V → **24 V AC, 30 W** (~1.25 A) |
| Logic power | LM2596 buck module with bridge rectifier, fed from the same 24 V AC, output **5.0 V** |
| Valves | 5× 24 V AC solenoid valve (normally closed) |

### Pinout

| ESP pin | GPIO | Function |
|---|---|---|
| D1 | 5 | Zone 1 → IN1 |
| D2 | 4 | Zone 2 → IN2 |
| D5 | 14 | Zone 3 → IN3 |
| D6 | 12 | Zone 4 → IN4 |
| D7 | 13 | Zone 5 → IN5 |
| D0 | 16 | Reserved: master valve/pump → IN6 (later) |
| RX | 3 | Reserved: flow meter (interrupt, later) — serial console must then be disabled |
| A0 | ADC | Reserved: rain sensor (later) |
| D3 | 0 | Onboard FLASH button: long press = emergency stop; very long = AP mode |
| D4 | 2 | Onboard blue LED (active LOW): status |
| D8 | 15 | **DO NOT USE** (must be LOW at boot; a relay input pulls it high → boot fails) |

Relays 7 and 8: spare.

### Wiring
- JD-VCC jumper **removed**. Relay `VCC` ← ESP `3V3`; `JD-VCC` ← 5 V buck; `GND` (5 V block) ← buck GND.
- ESP `VIN` ← 5 V buck, `GND` ← buck GND.
- 24 V AC wire A → buck input + COM of relays 1–5 (looped via Wago).
- Relay NO → valve 1–5. Common valve wires → 24 V AC wire B + other buck input.
- **Never** connect a 24 V AC wire to logic GND (shorts the bridge rectifier).
- When flashing over USB in the enclosure: preferably disconnect the 5 V.

### Hardware checks before first power-up
1. Set the buck output to 5.0 V **before** connecting the ESP/relay board.
2. Measure the unloaded DC voltage across the big capacitor: LM2596 max 40 V. Above 37 V: consider the HV variant.
3. Power budget: valve ~0.4 A inrush / ~0.25 A holding → max 2 valves at the same time.

---

## 2. Firmware – technical choices

- **PlatformIO**, Arduino core ESP8266, board `nodemcuv2`, flash layout 4M with ~1 MB LittleFS.
- Libraries (maintained forks):
  - `ESP32Async/ESPAsyncWebServer` + `ESP32Async/ESPAsyncTCP` (web server + WebSocket)
  - `bertmelis/espMqttClient` (MQTT, async) — phase 6
  - `bblanchon/ArduinoJson` v7
  - `ArduinoOTA` (OTA from PlatformIO)
- Config and schedules in **LittleFS** as JSON. Limit writes (flash wear).
- Time via NTP, default time zone `CET-1CEST,M3.5.0,M10.5.0/3` (configurable).
- **`include/secrets.h`** (in `.gitignore`): Wi-Fi SSID/password, OTA password, AP password, default admin password, default API key, MQTT host/user/password. `secrets.example.h` is the template. **Never commit secrets.**
- mDNS: `sqwrtr.local`. DHCP (fixed IP via DHCP reservation in the router).
- RAM is tight (~80 KB, 4 KB stack): web UI as vanilla HTML/CSS/JS, gzip, no frameworks. No large JSON buffers on the stack; stream big responses.
- Non-blocking code: no `delay()` in the main loop; everything via state machines/timers.
- Async web handlers run in the network context: change state only, never block or write flash there.

### Safe boot (critical)
- At boot: first `digitalWrite(pin, HIGH)` and **then** `pinMode(pin, OUTPUT)` → relays stay off (active LOW). Done in `preinit()` and again in `setup()`.
- After any (re)start: **all valves closed**, running programs aborted, event `boot` with reset reason.
- Hardware watchdog active; hang → reboot → everything closed.

### Updates (OTA)
1. First time via USB.
2. Then `ArduinoOTA` from PlatformIO (password protected; ESP connects back on TCP 8270).
3. Upload page `/update` (firmware `.bin` and LittleFS image). This page lives **in the firmware itself** (PROGMEM, `src/web_pages.h`), not in LittleFS, so recovery is always possible.
4. Later: update via Home Assistant (MQTT `update` entity; ESP fetches `.bin` from a URL supplied by HA/GitHub).
- The ESP8266 has no automatic rollback → OTA only allowed while no valve is open; self-test after update.

### Network robustness
- Wi-Fi lost → keep retrying, schedules keep running on local time.
- 5 min without Wi-Fi → **fallback access point** (`sqwrtr-setup`, with password) while still retrying; web UI reachable there too.
- MQTT lost → reconnect with backoff; everything keeps working locally.

---

## 3. ESP functionality

### Zones (5, count configurable for expansion)
Per zone: name, type (`lawn` / `border`, default `lawn`), enabled yes/no,
default duration, max runtime (default 120 min), runtime counters (total, per week, per season).

### Manual control
- Open a valve **for a set time** (minutes/seconds).
- Open a valve **indefinitely** → still capped by max runtime (safety limit).
- Close a valve, **emergency stop** (everything closed + running programs aborted).

### Safety rules
| Setting | Default |
|---|---|
| Max open at the same time | 2 (configurable) |
| Max runtime per valve | 120 min (configurable per zone) |
| Delay between switching valves on | 500 ms |
| Pause between zones in a program | configurable, default 5 s |

- Manual command while the limit is reached → **refused** (HTTP 409 + event `command_rejected`).
- Program steps that don't fit → **queue**.
- Winter mode → all commands and schedules blocked (except emergency stop).

### Programs (schedules run on the ESP)
- Max 8 programs. Per program: name, on/off, 1–4 start times, weekdays or every N days, list of steps `{zone, duration}` (sequential, max 10).
- Overlapping programs → queue, respecting max concurrent.
- **Seasonal factor** (0–200 %, global) and **factor per zone type** → scales all runtimes. Adjusted by HA.
- **Rain delay / skip**: "skip X hours" or "skip next run", with a reason (text) sent by HA.
- Run-now button per program.
- No scheduled starts until the clock is set (NTP); never start in the first minute after boot.

### Modes
`auto` (normal), `manual` (schedules off), `winter` (everything off), `test` (test cycle: each enabled zone 1 min, one after another; reported while running).

### Log
Ring buffer (200 entries) in RAM + periodically to LittleFS: time, zone/program, action, duration, source (`web` / `api` / `mqtt` / `schedule` / `button` / `console` / `system`), reason code.

### Status LED
Off = all ok/idle, slow blink = no Wi-Fi, fast = AP mode, on = valve open.

---

## 4. REST API

Base `/api`, JSON. Auth: header `X-API-Key: <key>` (or `Authorization: Bearer <key>`). Web UI: login with admin password → session cookie.

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/login` | body `{ "password": "..." }` → session cookie (only endpoint without auth) |
| POST | `/api/logout` | End session |
| GET | `/api/status` | Full status: zones, active runs, remaining time, queue, mode, rain delay, factor |
| GET | `/api/info` | Firmware version, uptime, free heap, RSSI, IP, reset reason |
| GET / PUT | `/api/zones`, `/api/zones/{id}` | Read/update zones |
| POST | `/api/zones/{id}/open` | body `{ "duration_s": 600 }` or `{}` for indefinite |
| POST | `/api/zones/{id}/close` | Close |
| POST | `/api/stop` | Emergency stop |
| GET / POST / PUT / DELETE | `/api/programs[/{id}]` | Manage programs |
| POST | `/api/programs/{id}/run` | Start program now |
| POST | `/api/skip` | body `{ "hours": 24, "reason": "rain expected" }`; `{ "next": true, "reason": "..." }` skips the next run; `{ "hours": 0 }` clears |
| PUT | `/api/mode` | `auto` / `manual` / `winter` |
| POST | `/api/test` | Test cycle |
| GET / PUT | `/api/settings` | Safety, factors, pause, time zone (MQTT in phase 6) |
| POST | `/api/settings/password` | body `{ "current": "...", "new": "..." }` (8–64 characters) |
| POST | `/api/settings/api-key` | New API key; only visible in this response |
| GET / POST | `/api/backup`, `/api/restore` | Zones + programs + settings as JSON (without password/API key) |
| GET | `/api/log` | Log (streamed, newest first) |
| POST | `/api/reboot` | Restart |
| WS | `/ws` | Live events and status push for the web UI |

Errors: consistent JSON `{ "error": "code", "message": "..." }`.

### Wire-value compatibility
Values stored in `/config.json` on devices must keep loading. Legacy Dutch values are accepted
on input as aliases: mode `handmatig` → `manual`, type `gazon` → `lawn`, days `ma…zo` → `mon…sun`,
setting `factor_gazon` → `factor_lawn`. The order of the `REASONS` table in `src/history.cpp`
must never change (indices are stored in `/log.bin`).

---

## 5. Events (push via MQTT + WebSocket)

`zone_opened`, `zone_closed` (with actual duration and reason), `program_started`, `program_finished`
(total runtime), `program_skipped` (reason), `command_rejected` (reason: limit / winter / ...),
`max_runtime_reached`, `emergency_stop`, `queue_changed`, `mode_changed`, `boot` (reset reason,
valves that were open at reboot), `wifi_fallback_ap`, `update_started`, `update_finished`, `update_failed`.
Detail keys: `zone`, `program`, `name`, `duration_s`, `indefinite`, `reason`, `source`, `mode`, `previous`, `queue`, `error`.
Later: `leak_detected`, `no_flow` (flow meter), `rain_detected` (rain sensor).

---

## 6. MQTT + Home Assistant discovery (phase 6)

- Base topic: `sqwrtr/<device_id>/...` (`device_id` = configurable, default `sqwrtr-<chipid>`) → ready for a 2nd controller.
- Availability/LWT: `sqwrtr/<id>/status` = `online` / `offline`.
- State: `sqwrtr/<id>/zone/<n>/state` (`open` / `closed`), `.../zone/<n>/remaining`, `sqwrtr/<id>/state` (JSON total).
- Commands: `sqwrtr/<id>/zone/<n>/set`, `.../cmd` (JSON: stop, skip, mode, run_program, test).
- Events: `sqwrtr/<id>/event` (JSON, not retained).
- Discovery under `homeassistant/...` with one device and entities:
  - `valve` per zone (open/closed)
  - `number` per zone (manual duration)
  - `sensor`: remaining time, active program, queue, runtime counters, RSSI, uptime
  - `select` mode
  - `number` seasonal factor
  - `button`: emergency stop, test cycle, start programs
  - `binary_sensor`: skip active
  - `update`: firmware

---

## 7. Web UI

- Mobile-first, light and fast, English. Dashboard with zone cards (status, remaining time, open-for-X / open / close), large emergency stop button, active program + queue, mode, rain delay.
- Pages: Dashboard · Programs (editor) · Zones · Log · Settings (safety, factors, password, API key, time zone, MQTT later) · System (info, update upload, restart, backup/restore config as JSON, wiring).
- Live updates via WebSocket.
- Source: `web/index.html` (single file, vanilla). `tools/build_web.py` turns it into a gzip PROGMEM header on every build (`include/web_ui.h`, not in git). The UI is part of the firmware: one OTA update updates everything. `/update` is separate, in `src/web_pages.h`.

---

## 8. Home Assistant side (phase 7)

- **Install the Mosquitto add-on** + MQTT integration.
- Division of roles: **the ESP runs the schedules, HA steers** (seasonal factor + skipping). If HA is down → watering continues.
- **Weather logic (own, Open-Meteo, free, no key)** via REST sensor:
  - Skip if forecast precipitation next 24 h > threshold (e.g. 3 mm) or fallen in the last 24 h > threshold.
  - Seasonal factor based on evapotranspiration (ET₀) and temperature; separate factor lawn vs borders/hedge.
  - Thresholds as `input_number` in HA, configurable.
- **Notifications (HA companion app)**: errors and safety, ESP offline, program start/end, skipped due to weather (with reason), firmware updates.
- **Maintenance**: winter mode, test cycle, runtime counters per valve, firmware update via HA.
- Deliver as an HA **package** (YAML) + dashboard YAML. Possibly a HACS integration later.

---

## 9. Build order

1. **Foundation**: PlatformIO project, secrets, safe pin init, Wi-Fi, mDNS, ArduinoOTA, serial logging. ✅
2. **Valve control + safety**: zone manager, limits, max runtime, stagger, emergency stop, FLASH button, LED. ✅
3. **REST API + auth + `/update` page** (recovery path). ✅
4. **Web UI** (dashboard, zones, settings, system) + persistent settings. ✅
5. **Programs**: NTP, scheduler, queue, factor, skip, persistence, log. ✅
6. **MQTT + HA discovery + events.**
7. **HA package**: Open-Meteo weather logic, notifications, dashboard, maintenance.
8. **Later**: firmware update via HA, HACS integration, flow meter (RX), rain sensor (A0), master valve/pump (D0), relays 7–8 / I/O expander, 2nd controller.

Each phase: compile, flash, test on hardware (without 24 V first, using the relay LEDs), only then continue.

## 10. Open points
- Zone names and lawn/border split: still unknown → configurable in the UI, default `lawn`.
- Unloaded DC voltage at the buck input still to be measured.
