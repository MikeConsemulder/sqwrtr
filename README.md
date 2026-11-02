# sqwrtr

**A self-hosted, stand-alone irrigation controller for ESP8266, with a mobile web UI, a REST API and Home Assistant integration via MQTT.**

sqwrtr ("squirter") turns a €5 ESP8266 board and a cheap relay module into a smart sprinkler controller for a manifold of 24 V AC solenoid valves. It runs its own schedules, keeps working when your Wi‑Fi or Home Assistant is down, and puts safety first: valves are closed on boot, runtimes are capped, and the number of valves open at the same time is limited to what your power supply can handle.

[![PlatformIO](https://img.shields.io/badge/built%20with-PlatformIO-orange)](https://platformio.org/)
[![ESP8266](https://img.shields.io/badge/MCU-ESP8266-blue)](https://www.espressif.com/en/products/socs/esp8266)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

---

## Contents

- [Why sqwrtr?](#why-sqwrtr)
- [Features](#features)
- [Project status](#project-status)
- [Hardware](#hardware)
- [Getting started](#getting-started)
- [Using the web UI](#using-the-web-ui)
- [How scheduling works](#how-scheduling-works)
- [Safety model](#safety-model)
- [REST API](#rest-api)
- [Live events (WebSocket)](#live-events-websocket)
- [Home Assistant](#home-assistant)
- [Configuration](#configuration)
- [Project structure](#project-structure)
- [Development](#development)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [License](#license)

---

## Why sqwrtr?

Commercial smart irrigation controllers are expensive, cloud-dependent and hard to extend. Home Assistant–only setups stop watering when your server reboots. sqwrtr sits in between:

- **The ESP runs the schedules.** Watering continues without Wi‑Fi, without Home Assistant and without the cloud.
- **Home Assistant steers, it doesn't drive.** HA can adjust a seasonal factor or skip runs based on the weather (over MQTT; a ready-made weather package follows in [phase 7](#project-status)), but if HA is gone, your garden still gets water.
- **Everything is local and open.** Web UI, REST API, WebSocket and OTA updates all run on the device itself. No accounts, no apps, no cloud.
- **Built for cheap, common hardware.** An ESP8266 dev board, an 8-channel relay module, a 24 V AC transformer and a buck converter.

## Features

**Valve control & safety**
- 5 zones out of the box (easy to change), open for a set time or "indefinitely" (still capped by a max runtime)
- Max valves open at the same time (default 2), max runtime per zone (default 120 min), 500 ms stagger between valve switch-ons
- Emergency stop from the UI, the API or the on-board FLASH button
- Safe boot: relays are forced off before anything else runs; valves that were open before a crash/watchdog reset are reported
- Firmware updates are refused while a valve is open

**Scheduling**
- Up to 8 programs, each with 1–4 start times, weekdays *or* "every N days", and up to 10 sequential steps
- Overlapping programs are queued, respecting the max-concurrent limit
- Seasonal factor (0–200 %) plus a separate factor for lawn and border zones
- Rain delay: skip the next X hours or just the next run, with a reason
- Modes: `auto`, `manual` (no automatic starts), `winter` (everything blocked), plus a one-click test cycle
- Time via NTP with a configurable POSIX time zone

**Interfaces**
- Mobile-first web UI (vanilla JS, ~12 KB gzipped, served from flash) with live updates over WebSocket
- REST API with API-key auth for scripts and Home Assistant
- Persistent log (last 200 events) and runtime counters per zone (this week / this season / total)
- Backup & restore of all settings as JSON
- OTA updates from PlatformIO or straight from the browser (`/update`)
- Fallback Wi‑Fi access point when your network is unavailable

## Project status

sqwrtr is built in phases. Each phase is compiled, flashed and tested on real hardware before moving on.

| Phase | Scope | Status |
|---|---|---|
| 1 | Foundation: PlatformIO project, safe pin init, Wi‑Fi + fallback AP, mDNS, ArduinoOTA | ✅ done |
| 2 | Valve control & safety: zone manager, limits, max runtime, stagger, emergency stop, button, LED | ✅ done |
| 3 | REST API, authentication, `/update` recovery page | ✅ done |
| 4 | Web UI, WebSocket live updates, persistent settings | ✅ done |
| 5 | Programs: NTP, scheduler, queue, factors, rain delay, modes, log, runtime counters | ✅ done |
| 6 | MQTT + Home Assistant auto-discovery + events | ✅ done |
| 7 | Home Assistant package: weather logic (Open-Meteo), notifications, dashboard | 🔜 next |
| 8 | Firmware update via HA, flow meter, rain sensor, master valve/pump, more zones | planned |

## Hardware

### Bill of materials

| Part | Notes |
|---|---|
| ESP8266 dev board | Developed on a **DOIT ESP-12F DevKit V3** (NodeMCU-style, 4 MB flash, CH340 USB-serial). Any NodeMCU v2/v3 works. |
| 8-channel relay module | 5 V coils (e.g. Songle SRD-05VDC-SL-C), **optocoupler inputs, active LOW**, with a **JD-VCC jumper**. |
| 24 V AC transformer | Powers the valves. Developed with a 30 W unit (~1.25 A). |
| Buck converter with rectifier | e.g. LM2596 module with bridge rectifier, fed from the same 24 V AC, set to **5.0 V** for the ESP and relay coils. |
| 24 V AC solenoid valves | Normally closed. Developed with 5. |

### Wiring

```
            24 V AC transformer
            wire A ───────────┬──────────────────────────► COM of relays 1–5
                              │
                              ▼
                   ┌──────────────────────┐
                   │ buck converter +     │  5.0 V +  ──► ESP VIN
                   │ bridge rectifier     │           ──► relay JD-VCC
                   └──────────────────────┘  5.0 V −  ──► ESP GND, relay GND (JD-VCC side)
                              ▲
            wire B ───────────┴──── common wire of all valves

  relay NO 1–5 ──► valve 1–5 (other valve wire = common, to wire B)
```

| ESP pin | GPIO | Connect to |
|---|---|---|
| D1 | 5 | Relay IN1 → zone 1 |
| D2 | 4 | Relay IN2 → zone 2 |
| D5 | 14 | Relay IN3 → zone 3 |
| D6 | 12 | Relay IN4 → zone 4 |
| D7 | 13 | Relay IN5 → zone 5 |
| 3V3 | – | Relay **VCC** (the pin on the IN1–IN8 header) |
| VIN / GND | – | Buck converter 5 V + / − |
| D0 | 16 | *Reserved:* master valve / pump → IN6 |
| RX | 3 | *Reserved:* flow meter |
| A0 | ADC | *Reserved:* rain sensor |
| D3 | 0 | On-board FLASH button (emergency stop / AP mode) |
| D4 | 2 | On-board blue LED (status) |
| D8 | 15 | **Do not use** — must be LOW at boot; a relay input pulls it HIGH and the board won't start |

**Remove the JD-VCC jumper.** The optocoupler side is then powered from the ESP's 3.3 V (so a 3.3 V HIGH reliably switches the relay *off*), while the relay coils get their own 5 V. With the jumper on and 5 V on VCC, a 3.3 V HIGH may not fully release the relay — a valve could stay open while the software thinks it's closed.

> ⚠️ **Never connect either 24 V AC wire to logic ground.** It shorts the bridge rectifier.
> ⚠️ Mains-powered transformers are dangerous. If you're not comfortable with mains wiring, use a pre-built plug-in transformer.

### Before first power-up

1. Set the buck converter output to **5.0 V** *before* connecting the ESP or relay board.
2. Measure the unloaded DC voltage after the rectifier. The LM2596 is rated for 40 V input; above ~37 V, use a high-voltage variant.
3. Power budget: a valve draws ~0.4 A inrush / ~0.25 A holding. With a 30 W transformer, keep **max 2 valves** open at once (the default).
4. Test without 24 V first: the relay LEDs show what the valves would do.

## Getting started

### Prerequisites

- [PlatformIO](https://platformio.org/) — either the VS Code extension or the CLI (`pip install platformio`)
- Python 3 (comes with PlatformIO; also used by the web-UI build step)
- A USB data cable and, on Windows, the [CH340 driver](https://www.wch-ic.com/downloads/CH341SER_EXE.html) if your board isn't recognised

### 1. Clone and configure

```bash
git clone https://github.com/MikeConsemulder/sqwrtr.git
cd sqwrtr
cp include/secrets.example.h include/secrets.h
```

Edit `include/secrets.h`:

| Define | What it is |
|---|---|
| `WIFI_SSID`, `WIFI_PASSWORD` | Your Wi‑Fi network |
| `OTA_PASSWORD` | Password for over-the-air updates from PlatformIO |
| `AP_PASSWORD` | Password of the fallback access point `sqwrtr-setup` (min. 8 characters) |
| `ADMIN_PASSWORD` | Initial web UI password (change it later in the UI) |
| `API_KEY` | Initial API key (generate a new one later in the UI) |
| `MQTT_HOST`, `MQTT_PORT`, `MQTT_USER`, `MQTT_PASSWORD` | Initial MQTT broker settings (use an IP address; change later in the UI). Leave `MQTT_HOST` empty to start with MQTT disabled |

`secrets.h` is in `.gitignore` and never committed.

### 2. First flash over USB

```bash
pio run -e usb -t upload
pio device monitor
```

(Use `python -m platformio` instead of `pio` if the command isn't on your PATH.)

You should see something like:

```
sqwrtr v0.5.0 – chip id ab07c7, free heap 47312
WiFi: connecting to 'my-wifi'...
WiFi: connected, IP 192.168.1.50, RSSI -49 dBm
mDNS: sqwrtr.local
OTA: ready
```

### 3. Open the web UI

Browse to `http://sqwrtr.local/` (or the IP address from the serial log) and log in with `ADMIN_PASSWORD`.

> Some browsers on Windows don't resolve `.local` names even when `ping sqwrtr.local` works. Use the IP address and give the device a fixed address (DHCP reservation) in your router.

### 4. Later updates (wireless)

Either upload `.pio/build/usb/firmware.bin` on **System → Firmware** (`/update`) in the web UI, or use ArduinoOTA:

```bash
# set upload_port in platformio.ini [env:ota] to your device's IP first
export SQWRTR_OTA_PASS=your-ota-password        # PowerShell: $env:SQWRTR_OTA_PASS = "..."
pio run -e ota -t upload
```

ArduinoOTA needs the ESP to connect *back* to your computer on TCP port 8270 — see [Troubleshooting](#troubleshooting) if it says "No response from device".

## Using the web UI

The UI is a single page with six tabs. A green dot top-right means the live connection is up.

| Tab | What you can do |
|---|---|
| **Dashboard** | Emergency stop · valves open / factor / clock · mode (Auto / Manual / Winter) · running program, step, time left, queue and next scheduled run · test cycle · rain delay (skip 24 h / skip next run / clear) · one card per zone: open for N minutes, open indefinitely, close |
| **Programs** | Create, edit, delete and start programs: name, enabled, start times, weekdays or every N days, and the steps (zone + minutes) |
| **Zones** | Per zone: name, type (lawn / border), default duration, max runtime, enabled; runtime counters for this week, this season and in total; which ESP pin and relay input it uses |
| **Log** | The last 200 events (valves, programs, skips, mode changes, emergency stops, boots), newest first |
| **Settings** | Safety (max valves at once, stagger) · seasonal factor and lawn/border factors · pause between zones · password · API key · time zone |
| **System** | Device info (version, IP, signal, uptime, memory, last reset reason) · firmware update · backup download / restore · restart · wiring overview |

### Status LED

| LED | Meaning |
|---|---|
| Off | All good, idle |
| Slow blink | No Wi‑Fi |
| Fast blink | Fallback access point active |
| On | A valve is open |

### FLASH button

| Hold for | Action |
|---|---|
| 2 s | Emergency stop (all valves closed, programs aborted) |
| 10 s | Start the fallback access point `sqwrtr-setup` (stays on for at least 15 min) |

## How scheduling works

- **Programs run on the ESP**, on local time. Every minute the scheduler checks whether a program should start today at this time.
- **Steps run one after another**, with a configurable pause between zones (default 5 s). A step for a disabled zone, or a step whose scaled duration is 0, is skipped.
- **Durations are scaled**: `actual = base × seasonal factor × zone-type factor`. A 10-minute lawn step with a seasonal factor of 80 % and a lawn factor of 125 % runs 10 minutes. The zone's max runtime still applies.
- **Concurrency and queue**: as many programs run side by side as the "max valves at once" setting allows (each running program uses one valve at a time). Others wait in a FIFO queue. Manually opened valves count towards the limit too.
- **Rain delay**: "skip X hours" blocks scheduled starts until a moment in time; "skip next run" swallows exactly one scheduled start. Both are logged with a reason. *Start now* and manual control ignore the rain delay.
- **Modes**: in `manual`, schedules don't start (manual control and *Start now* still work). In `winter`, every valve closes and every command is refused except the emergency stop. The *test cycle* opens every enabled zone for 1 minute, one after another.
- **No clock, no schedules.** Until the time has been set via NTP, scheduled programs don't start (manual control works). After a power cut without internet, schedules resume once the clock is set.
- **No double starts after a reboot.** In the first minute after boot the scheduler only observes; it never starts a program in that minute.

## Safety model

| Risk | What sqwrtr does |
|---|---|
| Relays clicking on during boot | Pins are driven HIGH (relay off) *before* they become outputs, already in `preinit()` |
| Crash or hang | Hardware + software watchdog reboot the ESP; after any reboot all valves are closed and the boot event lists valves that were open |
| Valve left open forever | Every opening — also "indefinite" — is capped by the zone's max runtime |
| Power supply overload | Max valves open at once (default 2) and a 500 ms stagger between switch-ons |
| Firmware update half-way through watering | OTA and `/update` are refused while any valve is open |
| Frost season | `winter` mode blocks everything except the emergency stop |
| Lost network | Schedules keep running on local time; after 5 min without Wi‑Fi a fallback AP starts |

## REST API

Base URL: `http://<device>/api`. All bodies and responses are JSON.

### Authentication

- **API key** for scripts and Home Assistant: header `X-API-Key: <key>` or `Authorization: Bearer <key>`.
- **Session cookie** for the web UI: `POST /api/login` with `{ "password": "..." }`.

The initial key and password come from `secrets.h`. Generate a new key on **Settings → API key** (it's shown once; only a salted hash is stored).

### Endpoints

| Method | Path | Description |
|---|---|---|
| POST | `/api/login` | `{ "password": "..." }` → session cookie (the only endpoint without auth) |
| POST | `/api/logout` | End the session |
| GET | `/api/status` | Everything at once: zones, mode, factors, rain delay, running programs, queue, next run, clock |
| GET | `/api/info` | Firmware version, uptime, free heap, RSSI, IP, reset reason, chip id |
| GET | `/api/zones` | All zones incl. configuration and runtime counters |
| GET / PUT | `/api/zones/{id}` | Read / update one zone (`name`, `type`, `enabled`, `default_duration_s`, `max_runtime_s`) |
| POST | `/api/zones/{id}/open` | `{ "duration_s": 600 }`, or `{}` for indefinite (capped by max runtime) |
| POST | `/api/zones/{id}/close` | Close a zone |
| POST | `/api/stop` | Emergency stop |
| GET / POST | `/api/programs` | List programs / create one |
| GET / PUT / DELETE | `/api/programs/{id}` | Read / replace / delete a program |
| POST | `/api/programs/{id}/run` | Start a program now (or queue it) |
| POST | `/api/test` | Start the test cycle |
| POST | `/api/skip` | `{ "hours": 24, "reason": "rain expected" }` · `{ "next": true, "reason": "..." }` · `{ "hours": 0 }` clears |
| PUT | `/api/mode` | `{ "mode": "auto" \| "manual" \| "winter" }` |
| GET / PUT | `/api/settings` | `max_concurrent`, `stagger_ms`, `season_factor`, `factor_lawn`, `factor_border`, `zone_pause_s`, `timezone`, `mqtt` (`enabled`, `host`, `port`, `user`, `password` (write-only), `device_id`, `discovery_prefix`) |
| POST | `/api/settings/password` | `{ "current": "...", "new": "..." }` (8–64 characters) |
| POST | `/api/settings/api-key` | Generate a new API key (returned once) |
| GET | `/api/log?limit=50` | Log entries, newest first (max 200) |
| GET | `/api/backup` | Zones, programs and settings as JSON (no password/API key) |
| POST | `/api/restore` | Restore a backup |
| POST | `/api/reboot` | Restart the device |
| WS | `/ws` | Live status and events (see below) |
| GET / POST | `/update` | Firmware / filesystem upload page (also accepts HTTP Basic `admin:<password>`) |

Errors always look like `{ "error": "limit_reached", "message": "Max 2 valves open at the same time" }` with a matching HTTP status (400, 401, 404, 409, 429, 500).

### Examples

```bash
KEY=your-api-key
HOST=http://192.168.1.50

# What's going on?
curl -H "X-API-Key: $KEY" $HOST/api/status

# Water zone 1 for 10 minutes
curl -X POST -H "X-API-Key: $KEY" -d '{"duration_s":600}' $HOST/api/zones/1/open

# Skip watering for 24 hours
curl -X POST -H "X-API-Key: $KEY" -d '{"hours":24,"reason":"rain expected"}' $HOST/api/skip

# Lower all runtimes to 70 %
curl -X PUT -H "X-API-Key: $KEY" -d '{"season_factor":70}' $HOST/api/settings

# Create a program: lawn front + back, Mon/Wed/Fri at 06:00
curl -X POST -H "X-API-Key: $KEY" $HOST/api/programs -d '{
  "name": "Lawn morning",
  "enabled": true,
  "starts": ["06:00"],
  "days": ["mon", "wed", "fri"],
  "steps": [{"zone": 1, "duration_s": 900}, {"zone": 2, "duration_s": 900}]
}'

# Every 3 days instead of weekdays
#   "every_days": 3, "from": "2026-05-01"

# Flash new firmware from the command line
curl -u admin:your-password -F file=@.pio/build/usb/firmware.bin $HOST/update
```

## Live events (WebSocket)

Connect to `ws://<device>/ws` (same authentication as the API — the session cookie or API key is checked during the handshake). You'll receive two kinds of messages:

```jsonc
{ "type": "status", "data": { /* same as GET /api/status */ } }   // on change, and every 15 s
{ "type": "event", "event": "zone_opened", "data": { "zone": 1, "duration_s": 600, "indefinite": false, "source": "web" } }
```

| Event | Data |
|---|---|
| `boot` | `version`, `reset_reason`, `valves_open_at_reboot` (zone numbers) |
| `zone_opened` | `zone`, `duration_s`, `indefinite`, `source` |
| `zone_closed` | `zone`, `duration_s` (actual), `reason`, `source` |
| `command_rejected` | `zone`, `reason` (`limit`, `disabled`, `winter`, `invalid_zone`), `source` |
| `max_runtime_reached` | `zone`, `max_s` |
| `emergency_stop` | `source` |
| `program_started` | `program`, `name`, `source` |
| `program_finished` | `program`, `name`, `duration_s`, `reason` (`done`, `aborted`, `emergency_stop`, `winter`, …) |
| `program_skipped` | `program`, `name`, `reason` (`rain_delay`, `skipped`, `manual`, `winter`, `already_running`) |
| `queue_changed` | `queue` (program ids) |
| `mode_changed` | `mode`, `previous`, `source` |
| `wifi_fallback_ap` | `reason` (`no_wifi`, `button`) |
| `update_started` / `update_finished` / `update_failed` | `type` / `bytes` / `error` |

`source` is one of `web`, `api`, `mqtt`, `schedule`, `button`, `console`, `system`. Program id `0` is the test cycle.

## Home Assistant

sqwrtr talks to Home Assistant over **MQTT with auto-discovery**: once it is connected to your broker, a device **sqwrtr** with all its entities appears in HA automatically — no YAML needed.

### Setup

1. In Home Assistant, install the **Mosquitto broker** add-on (Settings → Add-ons) and start it. The **MQTT** integration is then offered automatically; accept it.
2. Create a Home Assistant user for sqwrtr (Settings → People → Users, e.g. `sqwrtr`). Mosquitto accepts HA users as MQTT logins.
3. In the sqwrtr web UI go to **Settings → MQTT / Home Assistant**, enter the broker's **IP address** (the ESP cannot resolve `homeassistant.local`), port `1883`, user and password, and save. The status line turns green when connected. (Alternatively set `MQTT_*` in `secrets.h` before flashing.)
4. In HA: Settings → Devices & services → MQTT → **sqwrtr**.

The broker password is stored on the device (it has to be, to log in) but is never returned by the API and is not part of the backup download.

### Entities

| Entity | Type | Notes |
|---|---|---|
| `<zone name>` (per zone) | `valve` | Open/close; *open* uses the zone's duration below. Shows `opening` while waiting for the stagger delay |
| `<zone name> duration` (per zone) | `number` (min) | Duration used when opening from HA; same as the zone's default duration in the web UI |
| `<zone name> remaining` (per zone) | `sensor` (s) | Remaining run time |
| `<zone name> this week` / `this season` (per zone) | `sensor` (s) | Runtime counters |
| Mode | `select` | `auto`, `manual`, `winter`, `test` (selecting `test` starts the test cycle) |
| Season factor, Lawn factor, Border factor | `number` (%) | 0–200 %, scale all program run times |
| Rain delay | `binary_sensor` | On while a rain delay or "skip next run" is active; attributes: until, reason |
| Active program, Queue, Next run | `sensor` | |
| Emergency stop, Test cycle, Skip next run, Clear rain delay | `button` | |
| Run `<program name>` (per program) | `button` | Added and removed automatically when you create or delete programs |
| WiFi signal, Uptime | `sensor` | Diagnostic |

The device becomes **unavailable** in HA when sqwrtr is offline (MQTT last will). Renaming a zone or program in the web UI updates the entity names.

### MQTT topics

Base topic `sqwrtr/<device_id>`, where `device_id` defaults to `sqwrtr-<chipid>` (shown in the UI; change it there when you run more than one controller).

| Topic | Direction | Payload |
|---|---|---|
| `…/status` | out, retained | `online` / `offline` (last will) |
| `…/state` | out, retained | JSON: mode, factors, rain delay, runs, queue, next run, RSSI, uptime, runtime counters per zone |
| `…/zone/<n>/state` | out, retained | `open` / `opening` / `closed` |
| `…/zone/<n>/remaining` | out, retained | seconds (every 10 s while open) |
| `…/zone/<n>/duration` | out, retained | minutes |
| `…/event` | out | JSON event, e.g. `{"event":"zone_closed","zone":2,"duration_s":600,"reason":"timer","source":"schedule"}` (see [Live events](#live-events-websocket)) |
| `…/zone/<n>/set` | in | `OPEN` (zone duration), `CLOSE`, a number of seconds (`0` = until max runtime) or `{"duration_s":300}` |
| `…/zone/<n>/duration/set` | in | minutes |
| `…/mode/set` | in | `auto` / `manual` / `winter` / `test` |
| `…/factor/set`, `…/factor_lawn/set`, `…/factor_border/set` | in | 0–200 |
| `…/cmd` | in | JSON, see below |

`cmd` actions:

```json
{"action": "stop"}
{"action": "test"}
{"action": "run_program", "program": 2}
{"action": "skip", "hours": 24, "reason": "rain expected"}
{"action": "skip", "next": true, "reason": "rain expected"}
{"action": "skip", "hours": 0}
{"action": "skip_next"}
{"action": "clear_skip"}
{"action": "mode", "mode": "winter"}
{"action": "open", "zone": 3, "duration_s": 300}
{"action": "close", "zone": 3}
```

Retained command messages are ignored, so a stale retained `OPEN` can never open a valve after a reconnect. Rejected commands (limit reached, winter mode, …) show up as a `command_rejected` event.

Example automation, skipping watering when rain is forecast:

```yaml
automation:
  - alias: "sqwrtr: skip on rain"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rain_next_24h   # any forecast sensor you have
        above: 3
    action:
      - service: mqtt.publish
        data:
          topic: sqwrtr/sqwrtr-ab07c7/cmd   # your device id
          payload: '{"action":"skip","hours":24,"reason":"rain expected"}'
```

Phase 7 will ship this as a ready-made HA package: weather logic based on [Open-Meteo](https://open-meteo.com/) (free, no key) that skips watering after/before rain and adjusts the seasonal factor from evapotranspiration, plus notifications through the HA companion app.

### Without MQTT

You can also use the [REST API](#rest-api) from Home Assistant:

```yaml
# configuration.yaml
rest_command:
  sqwrtr_skip_24h:
    url: "http://192.168.1.50/api/skip"
    method: POST
    headers:
      X-API-Key: !secret sqwrtr_api_key
    payload: '{"hours": 24, "reason": "rain expected"}'
    content_type: "application/json"
```

## Configuration

### Compile time — `include/config.h`

| Setting | Default | Notes |
|---|---|---|
| `ZONE_PINS`, `ZONE_PIN_LABELS` | D1, D2, D5, D6, D7 | Add or remove entries to change the number of zones |
| `DEFAULT_MAX_CONCURRENT` | 2 | Valves open at the same time |
| `DEFAULT_STAGGER_MS` | 500 | Delay between switching valves on |
| `DEFAULT_MAX_RUNTIME_S` | 7200 | Max runtime per zone |
| `MAX_PROGRAMS`, `MAX_STEPS`, `MAX_STARTS` | 8, 10, 4 | Program limits |
| `DEFAULT_TZ` | `CET-1CEST,M3.5.0,M10.5.0/3` | POSIX time zone ([list](https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv)) |
| `WIFI_AP_FALLBACK_MS` | 5 min | Start the fallback AP after this long without Wi‑Fi |
| `CONSOLE_ENABLED` (build flag) | 1 | Serial test console; set to 0 when the flow meter uses RX |

### Runtime — stored on the device

Everything you change in the UI or through the API is stored in LittleFS and survives reboots and firmware updates:

| File | Content |
|---|---|
| `/config.json` | Zones, safety settings, programs, mode, factors, rain delay, time zone, MQTT settings, salted hashes of password and API key |
| `/log.bin` | Ring buffer with the last 200 log entries |
| `/stats.bin` | Runtime counters per zone |

Writes are delayed and batched to limit flash wear. Use **System → Backup** to download your configuration as JSON.

## Project structure

```
sqwrtr/
├── include/
│   ├── config.h              # pins, limits, defaults
│   └── secrets.example.h     # copy to secrets.h (git-ignored)
├── src/
│   ├── main.cpp              # setup/loop, safe boot, wiring of modules
│   ├── relays.*              # the only code that touches relay pins; RTC memory for crash reporting
│   ├── zones.*               # zone manager: open/close, limits, max runtime, stagger, emergency stop
│   ├── programs.*            # programs, scheduler, runs + queue, modes, factors, rain delay
│   ├── history.*             # log ring buffer + runtime counters (LittleFS)
│   ├── storage.*             # /config.json, password/API-key hashes, backup/restore
│   ├── ntp.*                 # time via NTP, time zone helpers
│   ├── net.*                 # Wi-Fi (with reconnect), fallback AP, mDNS, ArduinoOTA
│   ├── mqtt.*                # MQTT client, Home Assistant discovery, commands
│   ├── web.*                 # async HTTP server, WebSocket, /update upload
│   ├── api.*                 # REST API routes
│   ├── auth.*                # API key, sessions, Basic auth for /update
│   ├── events.*              # event bus → serial log + WebSocket + MQTT
│   ├── button.*, status_led.*, console.*
│   └── web_pages.h           # /update recovery page (PROGMEM)
├── web/
│   └── index.html            # the web UI (single file, vanilla JS)
├── tools/
│   └── build_web.py          # pre-build: gzips web/index.html into include/web_ui.h
├── platformio.ini
└── CLAUDE.md                 # design spec and project notes
```

## Development

### Architecture in short

- **Non-blocking everywhere.** No `delay()` in the main loop; every module has a `loop()` driven by timers and state machines.
- **One owner per resource.** Only `relays` touches relay pins, only `zones` decides when a valve opens, `programs` asks `zones` like any other client.
- **Async web server.** HTTP and WebSocket handlers run in the network context; they only change state and never block or touch the filesystem. Flash writes happen in `loop()`.
- **RAM is tight (~80 KB).** Large JSON is streamed (e.g. `/api/log`), the UI is a single gzipped file, and buffers live in static memory rather than on the 4 KB stack.

### The web UI

Edit `web/index.html` and build as usual — `tools/build_web.py` runs before every build and regenerates `include/web_ui.h` when the HTML changed. The UI is embedded in the firmware, so a single OTA update ships both. `/update` lives separately in `src/web_pages.h`, so you can always recover even if the UI is broken.

To iterate on the UI without flashing, serve `web/index.html` locally and mock `fetch`/`WebSocket`, or point it at a running device.

### Serial console

With `CONSOLE_ENABLED=1` (default) you can control the device over USB:

```bash
pio device monitor
```

```
status            zones and system
open <n> [sec]    open zone n (without sec: indefinite, capped by max runtime)
close <n>         close zone n
stop              emergency stop
wifi              Wi-Fi status and network scan
ap                start the fallback AP
reboot            restart
```

### Changing the number of zones

Add a GPIO to `ZONE_PINS` and its board label to `ZONE_PIN_LABELS` in `include/config.h`. Avoid GPIO 15 (D8), 0 (D3) and 2 (D4) — see the pin table. For more than ~7 zones, use an I/O expander (planned).

## Troubleshooting

| Problem | Solution |
|---|---|
| OTA: `getaddrinfo failed` | Python on Windows often can't resolve `.local` names. Put the IP address in `upload_port` of `[env:ota]`. |
| OTA: `No response from device` | Either a valve is open (updates are refused — close it first), or your firewall blocks the ESP connecting back on TCP port 8270. On Windows: `New-NetFirewallRule -DisplayName "sqwrtr OTA" -Direction Inbound -Protocol TCP -LocalPort 8270 -RemoteAddress LocalSubnet -Action Allow` (admin PowerShell), or set your network profile to Private. |
| `http://sqwrtr.local` doesn't load but ping works | Browser doesn't resolve mDNS — use the IP address. |
| A relay stays (half) on or flickers | Remove the JD-VCC jumper and wire VCC to 3.3 V, JD-VCC to 5 V (see [Wiring](#wiring)). |
| Programs don't start | Check the clock on **System** (NTP needs internet), the mode (must be `auto`), the rain delay, and that the program is enabled and has steps. |
| No COM port for USB flashing | Use a data-capable USB cable and install the CH340 driver. |
| Board doesn't boot with relays connected | Something is connected to D8 (GPIO 15). Move it. |
| Router shows the board online, but it's unreachable | Usually a mesh node refusing the association after the board moved. The firmware retries every 20 s; check the reason with the `wifi` console command or in the serial log (`connect failed (reason …)`). |
| MQTT stays "Not connected" | Use the broker's IP address (not `.local`), check user and password (the error is shown next to the status), and that port 1883 is reachable. |
| HA entities don't appear | Check that the MQTT integration is set up in HA and that the discovery prefix is `homeassistant`. |

## Contributing

Contributions are welcome — bug reports, hardware variants, translations, Home Assistant integration, you name it. See [CONTRIBUTING.md](CONTRIBUTING.md) for how to set up, the coding conventions and the pull-request checklist.

## License

[MIT](LICENSE) © Mike Consemulder

sqwrtr controls water and mains-powered equipment. It comes without any warranty; you are responsible for safe wiring and for what happens in your garden.
