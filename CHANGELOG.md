# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow the build phases.

## [Unreleased]

## [0.6.0] – MQTT + Home Assistant

### Added
- MQTT client (espMqttClient) with reconnect backoff and last will (`sqwrtr/<id>/status`).
- Home Assistant auto-discovery: valve, duration, remaining time and runtime counters per zone; mode select;
  season/lawn/border factors; rain delay binary sensor; active program, queue, next run, RSSI and uptime sensors;
  buttons for emergency stop, test cycle, skip next run, clear rain delay and each program.
- Commands over MQTT (`zone/<n>/set`, `zone/<n>/duration/set`, `mode/set`, `factor*/set`, `cmd` JSON); retained commands are ignored.
- All events are published to `sqwrtr/<id>/event`; full state as JSON on `sqwrtr/<id>/state`.
- MQTT settings in the web UI and `/api/settings` (`mqtt` object); MQTT status in `/api/info`.

### Changed
- Everything is now in English: web UI, log messages, API messages, code comments and documentation.
- API wire values translated: mode `handmatig` → `manual`, zone type `gazon` → `lawn`, day names `ma…zo` → `mon…sun`,
  setting `factor_gazon` → `factor_lawn`, event fields (`duur_s` → `duration_s`, `reden` → `reason`, `bron` → `source`, …),
  sources (`schema` → `schedule`, `knop` → `button`, `systeem` → `system`) and reason codes.
  Old values stored in `/config.json` are still accepted, so existing devices keep their settings.

### Added
- README, CONTRIBUTING, CHANGELOG and MIT license for the open-source release.
- Serial console command `wifi` (status + network scan); failed Wi-Fi connects are logged with their reason.

### Fixed
- Wi-Fi never reconnected after the access point refused an association (reason 203, seen on mesh networks):
  the firmware now restarts the connect attempt every 20 s while offline.
- Wi-Fi modem sleep disabled, so the device keeps answering ARP/ping.

## [0.5.0] – Programs
- NTP time with configurable POSIX time zone.
- Up to 8 programs: 1–4 start times, weekdays or every N days, up to 10 sequential steps.
- Scheduler, run queue respecting max concurrent valves, pause between zones.
- Seasonal factor and per-zone-type factors, rain delay (hours / next run), modes auto/manual/winter, test cycle.
- Persistent log (200 entries) and runtime counters per zone; streamed `/api/log`.

## [0.4.0] – Web UI
- Single-file web UI (gzipped into the firmware) with dashboard, zones, settings and system pages.
- WebSocket `/ws` for live status and events.
- Settings, zones and hashed password/API key persisted in LittleFS; backup/restore.

## [0.3.0] – REST API
- Async web server, REST API with API-key and session-cookie auth.
- `/update` page for firmware and filesystem uploads (only with all valves closed).

## [0.2.0] – Valve control & safety
- Zone manager: max concurrent valves, max runtime, stagger, emergency stop.
- FLASH button (emergency stop / AP mode), status LED, OTA blocked while a valve is open.
- Crash reporting of open valves through RTC memory, serial test console.

## [0.1.0] – Foundation
- PlatformIO project, safe relay initialisation in `preinit()`, Wi-Fi with fallback AP, mDNS, ArduinoOTA, serial logging.
