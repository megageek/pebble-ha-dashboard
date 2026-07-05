# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

This repo is for a **Pebble smartwatch watchface that displays data from a linked Home Assistant instance** (e.g. sensor states, temperatures, switch/light status), plus a few always-available local channels (time, date, battery, steps). A basic scaffold exists at the repo root (`package.json`, `wscript`, `src/c/main.c`, emery-only) built around a channel/slot/template system — see "Architecture" below. Not yet built: `src/pkjs/index.js` (HA WebSocket bridge), the Clay config page for channel/template selection, a second layout template, and gabbro support. Follow the skill below for build/test workflow.

## Building the watchface

Use the **pebble-watchface** skill (`.claude/skills/pebble-watchface/SKILL.md`, mirrored at `.agents/skills/pebble-watchface/`) to generate, build, and test this project. It defines the full research → design → implement → build → test → deliver flow and should be treated as the primary spec for how work in this repo proceeds. Key points not to rediscover from scratch:

- **Default/exclusive target platform is `emery`** (Pebble Time 2, 200x228, rectangular, 64-color). Round platforms (gabbro) are a second pass only if requested.
- **Battery rule: `tick_timer_service_subscribe(MINUTE_UNIT, ...)` only** — never `SECOND_UNIT` unless explicitly requested.
- **No floating point** — use `sin_lookup`/`cos_lookup`/`DEG_TO_TRIGANGLE`.
- Use `layer_get_bounds()` for screen dimensions; never hardcode 200x228 or other platform sizes.

### Commands
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

## Architecture: channels, slots, and templates

Three concepts are kept deliberately separate so layout and data source can vary independently:

- **Channel** — a named data source holding a `ChannelData` value (kind/style/unit/range/label), indexed by the `ChannelIndex` enum and populated in `init_channels()`: `CHANNEL_TIME`, `CHANNEL_DATE`, `CHANNEL_BATTERY`, `CHANNEL_STEPS` — all **local**, read straight off on-watch services (`battery_state_service_peek()`, `health_service_sum_today(HealthMetricStepCount)` gated by `health_service_metric_accessible()`), no phone/network round-trip — and `CHANNEL_HA_PLACEHOLDER`, standing in for a future **remote** HA-pushed channel until the pkjs bridge exists. Time and date are ordinary channels, not special-cased widgets, so they can be placed in any slot like any other channel.
- **Slot** — a screen position from a layout template: a `GRect` plus a `SlotSizeClass` (`SLOT_SIZE_HERO`, `SLOT_SIZE_MEDIUM`, `SLOT_SIZE_SMALL`). Size class, not the channel's kind, decides how the assigned channel is drawn — `HERO`/`MEDIUM` render a big/medium centered value with no label (`BITHAM_42_BOLD` / `GOTHIC_24`), `SMALL` renders a label+value stat row (label left, value right, a `draw_channel_bar()` progress bar instead of text when `style == CHANNEL_STYLE_BAR` and the channel has a range). The same channel renders differently depending which slot holds it.
- **Template** — a function (`build_template_0()` today) that, given the window bounds, returns an array of `SlotSpec { rect, size_class, channel }`. Only one template exists so far, reproducing the original fixed layout (hero=`CHANNEL_TIME`, medium=`CHANNEL_DATE`, three small rows=`CHANNEL_BATTERY`/`CHANNEL_STEPS`/`CHANNEL_HA_PLACEHOLDER`) through the new system instead of as hardcoded widgets. Slot→channel assignment is still hardcoded inside the template function; once the Clay config page exists, template choice and per-slot channel assignment both become user-configurable rather than fixed by array index.

Rendering: each slot is a plain `Layer` (not `TextLayer`), created with `layer_create_with_data(rect, sizeof(SlotRenderInfo))` storing `{channel_index, size_class}`. `slot_update_proc` reads that back and dispatches to `draw_channel_text_aligned()` (HERO/MEDIUM) or `draw_channel_small_row()` (SMALL). Any channel update — a minute tick, the battery callback, a steps refresh, or eventually an HA push — calls `mark_all_slots_dirty()` rather than tracking which slots currently show which channel; cheap with a handful of slots, and it stays correct automatically if slot→channel assignment changes later.

### Channel value model: kind, style, unit, range

Each channel's value is a small tagged struct (`ChannelData` in `src/c/main.c`), not a plain string, because channels vary in shape:

- **`kind`** — `CHANNEL_KIND_NUMERIC`, `CHANNEL_KIND_TEXT`, or `CHANNEL_KIND_BINARY`. Determines what the raw `value`/`text` field means (a number, a short string, or 0/1).
- **`unit`** — optional suffix for numeric channels (e.g. `"%"`), appended when rendered as text. Empty string means no unit.
- **`min`/`max`/`has_range`** — a numeric channel may declare a range for context/validation even if it's still shown as text (e.g. a temperature with a plausible range but no bar).
- **`style`** — `CHANNEL_STYLE_RAW` or `CHANNEL_STYLE_BAR`, an **explicit, independent choice** from whether a range exists. A ranged numeric channel still defaults to text unless `style` is set to `BAR`. This is deliberate: range and rendering style are decoupled so a future Clay config page can let the user pick "show as bar" per slot without that decision being baked into the data source. Bar rendering only ever applies when `kind == NUMERIC && has_range` (checked in `draw_channel_small_row()`); anything else falls back to text regardless of `style`, and `HERO`/`MEDIUM` slots always render as centered text since a bar wouldn't make sense there.
- Binary channels render as `"ON"`/`"OFF"` text today; a future pass can add an icon/dot style once a real binary source (e.g. an HA switch entity) exists to test against.

All bar math is integer-only (`(value - min) * width / range`), consistent with the skill's no-floating-point rule.

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
- **Channel → slot mapping is runtime config, not compiled in.** See the "channels, slots, and templates" section above — slot layout comes from a template, and which channel feeds which slot should eventually be user-configurable, not baked into `build_template_0()`. Use the Clay configuration framework pattern referenced by the skill (`tutorials/c-watchface-tutorial/part6` — fetched during the skill's research phase, not present in this repo yet) to build a config page that persists template choice + per-slot channel assignment and sends it to the watch as AppMessage keys (e.g. `SLOT_0_CHANNEL`, `SLOT_1_CHANNEL`).
- **Redraws are event-driven, not tick-driven, for HA data.** `src/c/main.c` still uses `tick_timer_service_subscribe(MINUTE_UNIT, ...)` for the clock itself, but channel values update the display by calling `layer_mark_dirty()` directly from `inbox_received_callback` whenever new channel data arrives — don't gate HA updates on the minute tick.
- **Reconnection is a phone-side concern.** The watch never holds a raw socket — pkjs owns the WebSocket lifecycle (auth, reconnect/backoff across phone app foreground/background transitions) and is the only thing that needs to handle dropped connections.
- Still register `app_message_register_inbox_received()` **before** `app_message_open()`, and still keep `"enableMultiJS": true` in `package.json`.
- The HA base URL and long-lived access token are user-specific config — do not hardcode a real token; treat it as something injected/configured on the phone side (e.g. via the Clay config page or a simple settings screen), same as the skill treats weather API access.
- `wscript` must use `pbl_build(..., bin_type='app')` with `js_entry_file='src/pkjs/index.js'` — the `pbl_program` pattern does not work with pkjs.
