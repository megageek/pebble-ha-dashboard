# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

This repo is for a **Pebble smartwatch watchface that displays data from a linked Home Assistant instance** (e.g. sensor states, temperatures, switch/light status). No watchface project has been generated yet — the repo currently only contains the `pebble-watchface` skill and an empty Python venv (uv-managed, no packages installed beyond pip/uv itself). The actual Pebble project (package.json, wscript, src/c/, src/pkjs/) is created by following the skill below.

## Building the watchface

Use the **pebble-watchface** skill (`.claude/skills/pebble-watchface/SKILL.md`, mirrored at `.agents/skills/pebble-watchface/`) to generate, build, and test this project. It defines the full research → design → implement → build → test → deliver flow and should be treated as the primary spec for how work in this repo proceeds. Key points not to rediscover from scratch:

- **Default/exclusive target platform is `emery`** (Pebble Time 2, 200x228, rectangular, 64-color). Round platforms (gabbro) are a second pass only if requested.
- **Battery rule: `tick_timer_service_subscribe(MINUTE_UNIT, ...)` only** — never `SECOND_UNIT` unless explicitly requested.
- **No floating point** — use `sin_lookup`/`cos_lookup`/`DEG_TO_TRIGANGLE`.
- Use `layer_get_bounds()` for screen dimensions; never hardcode 200x228 or other platform sizes.

### Commands (once the project exists)
```bash
pebble build                                   # compile, produces build/*.pbw
pebble install --emulator emery                # install to QEMU emulator
pebble logs --emulator emery                   # view device/emulator logs
pebble screenshot --no-open --emulator emery screenshot_emery.png
pebble emu-button click select --emulator emery
pebble login                                   # required before publishing
pebble publish                                 # publish to Pebble App Store
```
Asset generation scripts (from the skill):
```bash
python3 .claude/skills/pebble-watchface/scripts/create_app_icons.py .
python3 .claude/skills/pebble-watchface/scripts/create_preview_gif.py . --frames 8 --delay 400
```
There is no separate lint/test suite — correctness is verified by building, running in the QEMU emulator, and visually checking a captured screenshot with the Read tool (mandatory per the skill before calling anything "done").

## Architecture: Home Assistant data flow (push, channel-based)

Home Assistant data is not reachable from watch C code directly, so the same **AppMessage + PebbleKit JS bridge** the skill uses for weather still applies structurally — but the direction and shape of the data flow are different from the skill's default (polling) weather pattern. Do **not** copy the request/poll pattern from `templates/weather-watchface.c` / `templates/pkjs-weather.js` verbatim; adapt only the AppMessage plumbing, since HA **pushes** updates rather than the watch requesting them on a timer:

```
Home Assistant          Phone                              Watch
(custom integration/  ← │ src/pkjs/index.js │ ←AppMessage→ │ src/c/main.c │
 automation action)     │ persistent WS conn │
        │ fires event        │ relays on receipt
        └────────────────────┘
```

- **Transport is a persistent WebSocket, not REST polling.** `src/pkjs/index.js` opens a connection to Home Assistant's native `/api/websocket` endpoint (auth handshake with a long-lived access token), then subscribes to events fired by a custom HA integration/automation action rather than issuing periodic `GET`s. When an event arrives, pkjs relays it to the watch **immediately** via `Pebble.sendAppMessage()` — there is no `REQUEST_*` round-trip originating from the watch, and no fixed refresh interval to reproduce from the weather template.
- **Channels, not fixed entity fields.** Instead of hardcoding message keys per HA entity (e.g. `TEMPERATURE`), model a small fixed set of numbered **channels** (e.g. `CHANNEL_0_VALUE`/`CHANNEL_0_LABEL` … `CHANNEL_N_VALUE`/`CHANNEL_N_LABEL` as `messageKeys` in `package.json`). The HA-side custom integration/automation writes `{channel, value, label}` to whichever channel number it's configured for; the watchface doesn't know or care what entity backs a channel.
- **Channel → screen-slot mapping is runtime config, not compiled in.** The watchface layout has a fixed number of display slots (e.g. top stat, bottom stat, icon row). Which channel feeds which slot is user-configurable, not baked into `main.c`. Use the Clay configuration framework pattern referenced by the skill (`tutorials/c-watchface-tutorial/part6` — fetched during the skill's research phase, not present in this repo yet) to build a config page that persists the channel→slot mapping and sends it to the watch as its own AppMessage keys (e.g. `SLOT_0_CHANNEL`, `SLOT_1_CHANNEL`).
- **Redraws are event-driven, not tick-driven, for HA data.** `src/c/main.c` still uses `tick_timer_service_subscribe(MINUTE_UNIT, ...)` for the clock itself, but channel values update the display by calling `layer_mark_dirty()` directly from `inbox_received_callback` whenever new channel data arrives — don't gate HA updates on the minute tick.
- **Reconnection is a phone-side concern.** The watch never holds a raw socket — pkjs owns the WebSocket lifecycle (auth, reconnect/backoff across phone app foreground/background transitions) and is the only thing that needs to handle dropped connections.
- Still register `app_message_register_inbox_received()` **before** `app_message_open()`, and still keep `"enableMultiJS": true` in `package.json`.
- The HA base URL and long-lived access token are user-specific config — do not hardcode a real token; treat it as something injected/configured on the phone side (e.g. via the Clay config page or a simple settings screen), same as the skill treats weather API access.
- `wscript` must use `pbl_build(..., bin_type='app')` with `js_entry_file='src/pkjs/index.js'` — the `pbl_program` pattern does not work with pkjs.
