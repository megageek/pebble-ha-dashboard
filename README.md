# HA Dashboard — Pebble Watchface for Home Assistant

[![Test](https://github.com/megageek/pebble-ha-dashboard/actions/workflows/test.yml/badge.svg)](https://github.com/megageek/pebble-ha-dashboard/actions/workflows/test.yml)

A [Pebble](https://rebble.io/) smartwatch watchface that displays live data from a linked
[Home Assistant](https://www.home-assistant.io/) instance — sensor states, temperatures,
switch/light status — alongside always-available local channels (time, date, battery,
steps). Data flows both ways: Home Assistant pushes channel values to the watch over a
persistent WebSocket connection, and the watch reports its own battery, health, and
connection status back to Home Assistant.

Built around a **channel / slot / template** system so layout and data source vary
independently, with a [Clay](https://github.com/pebble-dev/clay) config page for
choosing the active layout and assigning any of 23 channels to any slot — no rebuild
required to reconfigure.

## Status

The watch/phone half of this project is implemented and tested. **Not yet built: the
Home Assistant–side custom integration.** `HA_INTEGRATION_SPEC.md` is the standalone
wire-protocol contract for that missing half, written to be dropped into whatever repo
eventually implements it. Round support (`gabbro`) is also a planned second pass — the
current build targets `emery` (Pebble Time 2) exclusively.

## Features

- **Runtime-configurable layout** — two built-in templates, 10 configurable slots, 23
  selectable channels (local + remote), no firmware rebuild needed to change what's
  shown or where.
- **Bidirectional Home Assistant bridge** — HA pushes up to 10 numbered channels to the
  watch (text, numeric, or binary; optional unit, range, bar-style rendering); the watch
  reports battery, step/activity/sleep/heart-rate health metrics, connection state, and
  device info back to HA, each report group independently toggleable.
  See [`HA_INTEGRATION_SPEC.md`](HA_INTEGRATION_SPEC.md) for the full wire contract.
- **Status dot group** — up to 4 small colored indicators overlaid on a slot for
  binary-ish states (a switch, a connection flag), independently colorable and
  hideable per channel. Any channel can be a dot source, local or Home Assistant.
- **Per-channel color overrides** — background, value, and label colors, configurable
  per local channel via Clay or pushed directly from Home Assistant for remote channels.
- **No floating point, minute-tick redraws** — battery-conscious by design; see
  `CLAUDE.md` for the specific constraints this codebase follows.

## Requirements

- [Pebble SDK](https://developer.rebble.io/developer.pebble.com/sdk/index.html)
  (`pebble-tool`), pinned to **SDK 4.17** for this repo — see
  [`.github/workflows/test.yml`](.github/workflows/test.yml).
- Node.js (20+) and npm, for the `pebble-clay` config page and pkjs bridge.
- A Pebble Time 2 (`emery`) device or the QEMU emulator bundled with `pebble-tool`.
- A Home Assistant instance with a long-lived access token, once the HA-side
  integration described in `HA_INTEGRATION_SPEC.md` exists.

## Getting Started

### Option A: Dev Container (recommended)

A ready-to-use [VS Code Dev Container](https://containers.dev/) is provided in
[`.devcontainer/`](.devcontainer/), pinned to SDK 4.17 and Node 20 to match CI.

1. Open this repository in VS Code with the
   [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers)
   installed (or use `docker compose up -d` directly from `.devcontainer/`).
2. Reopen in container when prompted. The container installs `pebble-tool`, activates
   SDK 4.17, and installs Node 20 automatically.

### Option B: Local install

```sh
pip install pebble-tool
pebble sdk install 4.17
pebble sdk activate 4.17
npm install
```

## Building & Testing

```sh
pebble build                                   # compile, produces build/*.pbw
pebble install --emulator emery                # install to the QEMU emulator
pebble logs --emulator emery                   # view device/emulator logs
pebble screenshot --no-open --emulator emery screenshot_emery.png

node test/ha_bridge.test.js                    # pkjs/HA bridge test suite (run after `pebble build`)
```

CI (`.github/workflows/test.yml`) runs `pebble build && node test/ha_bridge.test.js`
on every push and pull request.

## Configuration

The watchface ships with a Clay config page (`src/pkjs/config.js`) for choosing the
active layout template, assigning channels to slots, configuring the status dot group,
per-channel colors, and toggling which health/status metrics get reported to Home
Assistant. Home Assistant connection details (URL + long-lived access token) are
entered there too, and never leave the phone.

## Project Structure

```
src/c/main.c          Watch-side C: channels, slots, templates, rendering
src/pkjs/index.js      PebbleKit JS: Home Assistant WebSocket bridge, Clay wiring
src/pkjs/config.js      Clay config page definition
test/ha_bridge.test.js  Mock-WebSocket/Pebble test harness for the pkjs bridge
HA_INTEGRATION_SPEC.md  Wire-protocol contract for the (not-yet-built) HA-side integration
.devcontainer/         VS Code Dev Container definition
```

See [`CLAUDE.md`](CLAUDE.md) for a full architectural writeup — channel/slot/template
design, the HA data flow, status dot group, per-channel colors, and reporting toggles.

## License

MIT — see [`LICENSE`](LICENSE).
