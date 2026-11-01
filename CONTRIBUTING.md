# Contributing to sqwrtr

Thanks for wanting to help! sqwrtr is a small hobby project that controls real water valves, so changes are welcome but are held to one rule above all: **don't make it less safe.**

## Ways to contribute

- **Report a bug** — open an issue with what you did, what you expected, what happened, your board/relay module, the firmware version (System tab) and, if possible, the serial log (`pio device monitor`).
- **Share a hardware variant** — a different relay board, an ESP32 port, a 12 V DC setup, a pump/master valve. Photos and wiring notes help others.
- **Improve the docs** — the README, troubleshooting tips, translations.
- **Write code** — pick something from the roadmap in the README (MQTT/Home Assistant, flow meter, rain sensor, I/O expander) or fix an open issue. For anything bigger than a bug fix, open an issue first so we can agree on the approach.

## Development setup

```bash
git clone https://github.com/<you>/sqwrtr.git
cd sqwrtr
cp include/secrets.example.h include/secrets.h   # fill in your Wi-Fi etc.
pio run -e usb                                     # build
pio run -e usb -t upload && pio device monitor     # flash + serial log
```

You don't need valves or 24 V to develop: an ESP8266 board plus the relay module (or even just the board — watch the serial log) is enough. Test without 24 V first; the relay LEDs show what the valves would do.

The web UI is `web/index.html`. It's gzipped into the firmware automatically by `tools/build_web.py` on every build.

## Coding conventions

- **C++ (Arduino core for ESP8266)**, 4-space indent, ~110 columns, same style as the surrounding code.
- **Non-blocking:** no `delay()` in `loop()` paths; use `millis()` timers and small state machines.
- **One owner per resource:** only `relays` writes relay pins; only `zones` opens/closes valves. Other modules (programs, API, button, MQTT) ask `zones`.
- **Async context:** HTTP/WebSocket handlers run in the network context. Keep them short, never block, never write to flash there — set state and let `loop()` do the work.
- **RAM is precious (~80 KB total, 4 KB stack):** stream large responses, prefer static buffers over big stack arrays, avoid `String` in hot paths, check the RAM figure in the build output.
- **Comments explain why**, not what. English everywhere (code, UI, logs, API).
- **API/wire names are a contract** with the web UI (and soon Home Assistant): don't rename JSON keys, event names, reason codes or modes without updating every consumer, and keep accepting old values when they may be stored on devices (`/config.json`).
- **Persisted formats** (`/config.json`, `/log.bin`, `/stats.bin`) must stay backwards compatible, or migrate them explicitly.

## Safety checklist for pull requests

If your change touches valves, timing, boot, OTA or limits, confirm in the PR:

- [ ] Relays stay off during boot and after any reset (tested by resetting, power-cycling and OTA-updating with the relay board connected)
- [ ] Every way to open a valve still goes through `zones::open()` and respects max runtime, max concurrent and stagger
- [ ] Emergency stop (UI, API, button) still closes everything and aborts programs
- [ ] OTA/`/update` are still refused while a valve is open
- [ ] Builds without new warnings; RAM/flash usage noted if it changed noticeably

## Pull requests

1. Fork, create a branch (`feature/mqtt-discovery`, `fix/queue-order`, …).
2. Keep PRs focused — one feature or fix per PR.
3. Describe what you changed, why, and how you tested it (on hardware if relevant).
4. Update the README / API docs if behaviour or the API changes, and add a line to `CHANGELOG.md`.

By contributing you agree that your contributions are licensed under the [MIT License](LICENSE).
