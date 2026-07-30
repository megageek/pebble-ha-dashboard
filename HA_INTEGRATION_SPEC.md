# Home Assistant Integration Spec

**This is the contract the Pebble watchface's phone-side bridge (`src/pkjs/index.js`) expects from Home Assistant, in both directions.** It exists to be copied into whatever repo builds the actual HA-side custom integration — that side isn't implemented anywhere in *this* repo, only consumed. If you change the wire protocol here (command names, channel count, payload shapes, truncation limits, reported fields), update this file in the same commit; it's the only place the contract is written down independent of the watch/phone source.

Two independent flows share the one WebSocket connection:
1. **HA → watch**: channel data (`subscribe_channels`), covered first below.
2. **Watch → HA**: battery/health/device status reports (`report_status`), covered after that.

*Last synced with `pebble-ha-dashboard` commit `192bf80` (2026-07-06).*

**This requires a real custom integration, not just automations.** Home Assistant's generic event bus (`fire_event`/`subscribe_events`) is no longer used — instead the integration registers its own WebSocket API command. That means someone needs to write a `custom_components/<domain>/` Python package with a `websocket_api.async_register_command` handler; this can't be wired up purely through the UI/YAML automations the way a "fire an event on state change" approach could.

## Transport

Home Assistant's native WebSocket API — the same connection and session Home Assistant already provides, not a separate server or port: `ws(s)://<host>:<port>/api/websocket`. The phone (pkjs) is the client; it never polls, it holds one persistent connection and reconnects with exponential backoff (2s → 30s, phone-side concern, nothing HA needs to do about it) if dropped.

Auth is a standard HA long-lived access token, sent through HA's normal WebSocket auth handshake:

1. Server → client: `{"type": "auth_required"}`
2. Client → server: `{"type": "auth", "access_token": "<token>"}`
3. Server → client: `{"type": "auth_ok"}` or `{"type": "auth_invalid", "message": "..."}`

**No admin scope required.** Custom WebSocket API commands are not admin-gated unless the integration explicitly adds `@websocket_api.require_admin` — don't add that decorator, since a regular token should be sufficient. (This is a deliberate improvement over an earlier design that used HA's `fire_event` command, which *is* admin-only.)

## What the phone does after auth_ok

Every time auth succeeds — both the very first connection and every reconnect after a drop — the phone sends exactly one command:

```json
{"id": 1, "type": "pebble_dashboard/subscribe_channels"}
```

`pebble_dashboard/subscribe_channels` is a **custom command name the integration must register** — it is not a core HA command. The exact string must match `HA_SUBSCRIBE_COMMAND` in `src/pkjs/index.js`; if the integration's domain or command name differs, update one side or the other so they agree. `id` is a per-connection request counter the phone assigns (starts at 1, increments); the integration doesn't need to validate it beyond echoing it back in replies, per normal HA WebSocket API conventions.

This single command does the job that used to take two round trips: it both fetches current state and subscribes to future changes.

## What Home Assistant must do

### 1. Register `pebble_dashboard/subscribe_channels` as a WebSocket API command

Using `homeassistant.components.websocket_api.async_register_command`. On receiving it:

1. **Reply immediately with current state** as the command result:
   ```json
   {"id": 1, "type": "result", "success": true, "result": {"channels": [
     {"channel": 1, "value": "21.5°C", "label": "Temp"},
     {"channel": 2, "value": "Closed", "label": "Door"}
   ]}}
   ```
   Only include channels that are currently mapped to something; an unmapped channel can simply be omitted from the array (the watch keeps its placeholder/last-known value for it).
2. **Keep the connection subscribed** (standard HA pattern: register an unsubscribe callback via `connection.subscriptions[msg["id"]]`, cleaned up automatically when the connection closes) and **push future changes** as they occur, tagged with the *same* request `id` the phone sent:
   ```json
   {"id": 1, "type": "event", "event": {"channel": 1, "value": "22.0°C", "label": "Temp"}}
   ```
   (This is HA's standard `websocket_api.event_message(msg_id, payload)` helper — same shape any custom subscription command uses, just with our own `channel`/`value`/`label` payload instead of a core event.)
3. **If the command can't be serviced** (e.g. misconfigured), reply with `{"id": 1, "type": "result", "success": false, "error": {...}}` — the phone logs this and does not retry the command on the same connection; it'll simply get no data until the next reconnect (see below) or an integration fix.

If the integration isn't installed at all, HA responds to any unrecognized command with `{"success": false, "error": {"code": "unknown_command", ...}}` automatically — no special handling needed on the integration's part for that case, but the phone-side log for it is your first debugging signal that the integration isn't loaded/registered.

### 2. Payload shape (both the initial `result.channels[]` entries and each `event`)

```json
{
  "channel": 1,
  "value": "21.5°C",
  "label": "Temp",
  "on_color": "red",
  "off_color": "green",
  "hide_when": "off",
  "bg_color": "blue",
  "value_color": "white",
  "label_color": "gray",
  "kind": "numeric",
  "unit": "C",
  "min": 0,
  "max": 40,
  "style": "bar"
}
```

- **`channel`** — integer `1` through `10`. There are exactly **10 channels** in the current watchface (`CHANNEL_HA_1`…`CHANNEL_HA_10` on the watch side). Any other value (0, 11+, non-numeric) is silently ignored by the phone — not an error, just dropped.
- **`value`** — required. Its **wire type**, not `kind` below, is what the watch uses to tell text from numeric/binary: send a string for a text channel (the default if `kind` is never sent — the watch displays exactly what's sent, with no unit suffix or number formatting applied), or a plain integer number for a numeric/binary channel (see `kind` below). **If this channel is ever displayed as a status dot** (see below — entirely the watch user's choice, not something HA controls) *and* it's a text channel, send the literal lowercase string `"on"` or `"off"` so the watch can tell which dot color to use; any other value is always treated as "off" for dot purposes, no case-insensitive or fuzzy matching. A numeric/binary channel's dot state instead comes from whether `value` is zero.
- **`label`** — optional. If omitted, the watch keeps showing whatever label it last had for that channel (including the compiled-in default, e.g. `"HA1"`, if none has ever been sent). Only send it when it changes or on the first update for a channel.
- **`kind`** — optional, `"text"` (default), `"numeric"`, or `"binary"`. **Sticky** on the watch — like `label`, it only needs to be (re)sent when it changes, not on every `value` update; if never sent but `value` arrives as a number anyway, the watch assumes `"numeric"` rather than showing nothing. `"numeric"` renders the number with an optional `unit` suffix (and, with `min`/`max`/`style`, optionally as a progress bar); `"binary"` renders `"ON"`/`"OFF"` text. **Values must be whole integers — no floats.** The watch (like the rest of this project) does no floating-point math; pre-round/scale on the integration side (e.g. send `22`, not `21.5`, for a 22°C reading — if you need fractional precision, scale it into the `unit` string instead, e.g. `value: 215, unit: "0.1°C"` is a valid workaround if whole-degree precision genuinely isn't enough, though most sensors are fine rounded to whole units).
- **`unit`** — optional, only meaningful for `kind: "numeric"`. A short suffix appended after the number, e.g. `"C"`, `"%"`, `"kcal"` — no space is inserted, include one in the string if you want it (e.g. `"unit": " kcal"`).
- **`min`/`max`** — optional, only meaningful for `kind: "numeric"`, and only take effect when **both** are sent in the *same* message — sending just one is silently dropped, not applied half-set. Declares a range for context/validation and, combined with `style: "bar"`, enables progress-bar rendering. This is metadata about the channel, not a live value — send it once when the channel is first configured, or whenever the range itself changes (e.g. a thermostat's configurable min/max), not on every reading.
- **`style`** — optional, `"raw"` (default) or `"bar"`, only meaningful once `kind: "numeric"` and a range (`min`/`max`) both exist — same as the local `CHANNEL_STYLE_BAR` used for the built-in battery channel. Has no effect in a HERO/MEDIUM-sized slot (those always render centered text, same restriction as local channels), only in the small stat-row slots.
- **`on_color`/`off_color`** — optional, only relevant if the watch user has put this channel in their status-dot group (see below). A name from the shared palette: `red`, `orange`, `yellow`, `green`, `blue`, `purple`, `white`, `gray`. Unrecognized/misspelled names fall back to gray rather than erroring. If never sent, the watch defaults both to green/red.
- **`hide_when`** — optional, `"none"` (default — always show the dot), `"on"`, or `"off"`. Lets a "problem" indicator stay invisible until it actually needs attention (e.g. `hide_when: "off"` on a door-open sensor so the dot only appears when the door actually opens) rather than showing a "everything's fine" dot all the time.
- **`bg_color`/`value_color`/`label_color`** — optional, general-purpose styling that applies to this channel's *normal* rendering wherever it's assigned, not just when it's a status dot (see "Channel colors" below). Same shared palette as `on_color`/`off_color`. If never sent, the watch falls back to its own default colors for that slot type (white/light-gray text on the plain black background), same as it always has.
- **Truncation, enforced phone-side, not HA-side**: `value` (text channels) is cut to **15 characters**, `label` to **7 characters**, `on_color`/`off_color`/`bg_color`/`value_color`/`label_color` to **7 characters**, `hide_when` to **4 characters**, `kind`/`unit` to **7 characters**, `style` to **3 characters**, before being relayed to the watch (hard limits of the watch's fixed-size buffers). Truncation is a dumb substring cut, not word-aware — prefer sending values that are already short (e.g. `"21.5°C"` rather than `"21.5 degrees Celsius"`) rather than relying on the phone to truncate sensibly. `value`/`min`/`max` for numeric/binary channels are plain integers, not subject to string truncation.

## Status dots (optional, watch-side display feature)

The watchface can show up to 4 small colored dots inside one screen slot, **in addition to** whatever that slot's normal channel shows — for binary-ish measures that don't need a whole slot of their own. **This is entirely a watch/Clay-side display choice** — which slot, which channels feed the dots, is all configured on the watch, not by HA. The integration's only role is optionally supplying `on_color`/`off_color`/`hide_when` (above) so a channel looks reasonable *if* the user chooses to display it as a dot — there's no way for HA to force a channel to be shown as a dot, and no need to tell HA which channels currently are.

If your integration doesn't care about dot styling, it can simply never send `on_color`/`off_color`/`hide_when` — the watch has its own defaults (green="on"/red="off", always visible) and dot rendering works fine without any HA-side involvement.

Dot **size** (small/medium/large) is also a pure watch/Clay-side setting, like slot/channel assignment — HA never sends or receives it and has no visibility into what size the user picked.

## Channel colors (background/value/label, optional)

Separately from dot styling, every channel — including each of the 10 HA channels — can also have a background color for its slot, a foreground color for its value text, and a foreground color for its label text, applied to its *normal* rendering (not just when it happens to be a status dot). For HA channels these are exactly the `bg_color`/`value_color`/`label_color` fields on the payload above — there's no separate command for this, it's just three more optional fields on the same `pebble_channel_update`-shaped message.

- **Which slot shows a channel, and whether that slot is HERO/MEDIUM/SMALL-sized, is still entirely a watch/Clay-side choice** — HA only ever styles *its own channels*, the same way it already can via `on_color`/`off_color`. There's no way for HA to affect a local (non-HA) channel's colors, or to know or care which slot number a channel currently occupies.
- **`label_color` only has a visible effect if the channel ends up in a small stat-row slot** (`SLOT_SIZE_SMALL` on the watch side) — the only slot size that renders a label at all. If the watch user has put this channel in the HERO or MEDIUM position instead, `label_color` is accepted but has nothing to apply to; no need to avoid sending it defensively.
- **Omit any of the three to leave that part unstyled** — the watch falls back to its own normal default for that slot type (a plain black background, white or light-gray text depending on slot size) exactly as if this feature didn't exist. There's no need to resend all three together; each is independently optional, same as `on_color`/`off_color`/`hide_when`.
- **Not persisted on the watch** — same as the dot on/off colors, HA is the source of truth for its own channels' styling. If you want a channel's colors to survive a watch restart/reconnect, resend them (they'll naturally be included in whatever `value`/`label` update triggered the change) — the watch will otherwise just show its all-default appearance for that channel until the next update arrives.

## Watch → HA: status reports (battery, health, device info)

Separately from channel data (which flows HA → watch), the watch reports its own status back to HA over the same connection, as independent commands of the same type — never combined into one message, not even at startup: a periodic status report (battery/health), a one-time device-info report (model/firmware/color) sent once at app startup, and a phone-native connection-status report (see its own section below). All are relayed as separate `pebble_dashboard/report_status` commands:

```json
{"id": 2, "type": "pebble_dashboard/report_status", "status": {
  "battery_percent": 87,
  "battery_charging": 0,
  "steps": 4213,
  "active_seconds": 1800,
  "distance_meters": 3120,
  "active_kcal": 210,
  "resting_kcal": 2918,
  "sleep_seconds": 0,
  "sleep_restful_seconds": 0,
  "heart_rate_bpm": 72,
  "disabled": "heart_rate,sleep"
}}
```

And separately, sent exactly once, at app startup only:

```json
{"id": 3, "type": "pebble_dashboard/report_status", "status": {
  "model": "Pebble Time 2",
  "firmware": "4.4.1",
  "color": "Black"
}}
```

Register this as a second WebSocket API command, same as `subscribe_channels`. It doesn't need to be a *subscription* — a plain command handler that reads `msg["status"]` and does whatever the integration wants with it (create/update sensors, log it, etc.) and replies with `connection.send_result(msg["id"])` (no data needed in the reply) is sufficient. The phone doesn't wait on or use the ack for anything — treat it as fire-and-forget from the phone's side.

Field notes:
- **Every field is optional** — only present if that metric was actually accessible on the watch at report time (e.g. `heart_rate_bpm` is omitted entirely on a watch with no HR sensor, or if the user hasn't granted Health permission on their phone) *or* if the user disabled it in the watchface's config — see `disabled` below for how to tell those two cases apart. Don't assume any field is always there.
- **A periodic report is only actually sent when something in it has changed since the last one** — the watch keeps a snapshot of the last report it sent (every field's value, and whether each optional group was present at all) and compares before sending. HA already keeps an entity's last-known value until a new one arrives, so re-sending identical data on every trigger was a pure battery cost with no informational value. When a report *is* sent (because something differs), it still carries the fields for **every currently-enabled measure group**, not just the one that changed — there is no per-trigger *partial* subset, only a per-trigger *send-or-skip* decision. A report is triggered (and then sent only if it actually differs from the last one): once at app startup, on every battery-state change, every 5 minutes on the clock tick (not every minute — steps/activity/heart rate drift almost every single minute while the watch is worn, which defeated the change-detection dedup above when checked that often; 5 minutes is a cadence that doesn't need to be finer), and — unconditionally, bypassing the change check — immediately whenever the user changes a reporting toggle (see below).
- One consequence of only sending on change: if a report that *would* have changed something (e.g. a `disabled` toggle) is lost in transit, and nothing else changes afterward, HA won't learn about it until the next report that has something genuinely new to say. This is considered an acceptable tradeoff for the battery savings; treat `report_status` as reflecting the watch's state as of its last successful send, not as a guaranteed-fresh heartbeat.
- `battery_percent` (0–100), `battery_charging` (0/1).
- `steps`, `active_seconds`, `distance_meters` (integer meters), `active_kcal`, `resting_kcal`, `sleep_seconds`, `sleep_restful_seconds` are **today's cumulative totals** (reset at local midnight on the watch) — not deltas, not per-event.
- `heart_rate_bpm` is the watch's most recent instantaneous heart-rate sample, taken from whatever the watch's **own default background HR sampling** already produced (`health_service_peek_current_value()`, no elevated sampling requested) — so it can be up to ~15 minutes stale per Pebble's own docs. This is deliberate: no extra battery cost is spent to keep it fresher.
- `model`, `firmware`, `color` are static device info, sent **once at app startup only**, always as their own separate `pebble_dashboard/report_status` message — never combined with the periodic fields above, not even in the very first messages sent at startup (see the two example payloads above). Toggling `device_info` off/on later never resends this; only the `disabled` field's membership changes.
- **`disabled`** — a comma-separated string naming every measure-group the user has explicitly turned off in the watchface's Clay config (values: `battery`, `steps`, `activity`, `sleep`, `heart_rate`, `device_info`), e.g. `"heart_rate,sleep"`. **Absent entirely if nothing is disabled.** This is the signal to actually remove/deactivate an entity, as distinct from a field that's merely absent because it's not currently accessible on the hardware (which should probably just mark an existing entity unavailable, not delete it) — `disabled` is included on *every actual send* (not just once at the moment of toggling), and toggling a group always forces a send even if nothing else changed, so it stays in sync; treat it as the authoritative current set as of the last report, not an edge-triggered event.
- **Only sent once the phone is authenticated to HA.** If the phone hasn't connected yet (HA_URL/HA_TOKEN not configured, or mid-reconnect), a report is simply dropped rather than queued — the next report that actually has something to send will try again once connected. There's no retry/backfill for missed reports. A report is also sent **immediately** (not waiting for the next 5-minute tick) whenever the user changes a reporting toggle, so a disablement reaches HA promptly.

## Watch → HA: connection status (phone-native, not relayed from the watch)

`connected` used to be part of the periodic report above, sent by the watch. It was removed: a `connected` field the watch has to successfully transmit in order for HA to receive it is close to meaningless — the one time it would actually be useful to say (the watch just disconnected) is exactly the moment that message could never arrive. The phone's own OS/Bluetooth layer knows about a disconnect independently of the watch's cooperation, so connection status is now determined and sent entirely by the phone (`src/pkjs/index.js`'s `checkWatchConnection()`), as its own minimal `report_status` message:

```json
{"id": 4, "type": "pebble_dashboard/report_status", "status": {"connected": 1}}
```

- Sent once right after every successful HA authentication (startup and every reconnect), and then re-checked every 30 seconds (`WATCH_CONNECTION_POLL_MS`) — but, same change-detection principle as the periodic report above, only actually sent to HA when the value differs from what was last successfully sent.
- Sourced from `Pebble.getActiveWatchInfo()` (truthy = connected), which is pure local JS state the Pebble mobile app already maintains — no AppMessage, no Bluetooth radio wake, effectively free compared to anything watch-initiated.
- **Not a proven-reliable mechanism yet.** Whether `Pebble.getActiveWatchInfo()` reliably reflects real disconnects across every phone platform and Pebble app version hasn't been confirmed on real hardware as of this writing. If it turns out not to, this whole mechanism may be removed rather than patched — don't build HA-side logic that assumes `connected` transitions are prompt or reliable.
- There is no `REPORT_ENABLE_CONNECTED` toggle (or any Clay control) for this — it isn't part of the per-measure reporting toggles below, since it doesn't come from the watch's own reporting path at all.

## Per-measure reporting toggles (watch-side config, not part of the wire protocol)

Each measure group (`battery`, `steps`, `activity`, `sleep`, `heart_rate`, `device_info`) can be turned off by the user via the watchface's own Clay config page — this is entirely internal to the watch/phone side (`REPORT_ENABLE_*` AppMessage keys, Clay toggles), **the integration doesn't configure or query this**, it only ever observes the effect via the `disabled` field in `report_status` described above. There's nothing for the HA-side integration to implement here beyond correctly reacting to `disabled`. (Connection status has no toggle — see the section above.)

## What's *not* in this contract

- **No entity-to-channel mapping convention is prescribed here.** How a specific HA entity gets assigned to a channel number (a config flow option, YAML, a dashboard helper, whatever) is entirely up to the integration's design — the watch and phone only know about channel numbers 1–10, never entity IDs.
- **No explicit unsubscribe command from the phone.** The phone never sends one; it just closes/replaces the connection (e.g. on reconnect or when Home Assistant config is re-saved). The integration should treat connection close as the unsubscribe signal, per HA's standard subscription cleanup pattern, rather than expecting an explicit unsubscribe message.

## Quick reference

| Direction | Message | Shape | Notes |
|---|---|---|---|
| Phone → HA | `pebble_dashboard/subscribe_channels` command | `{id, type}` | Sent once per successful auth (startup + every reconnect) |
| HA → phone | `result` (reply to the command above) | `{id, type: "result", success, result: {channels: [{channel, value, label?, kind?, unit?, min?, max?, style?, on_color?, off_color?, hide_when?, bg_color?, value_color?, label_color?}, ...]}}` | Current state, sent once, immediately |
| HA → phone | `event` (tagged with the same `id`) | `{id, type: "event", event: {channel, value, label?, kind?, unit?, min?, max?, style?, on_color?, off_color?, hide_when?, bg_color?, value_color?, label_color?}}` | Every subsequent change, ongoing |
| Phone → HA | `pebble_dashboard/report_status` command (periodic) | `{id, type, status: {battery_percent?, battery_charging?, steps?, active_seconds?, distance_meters?, active_kcal?, resting_kcal?, sleep_seconds?, sleep_restful_seconds?, heart_rate_bpm?, disabled?}}` | Triggered at startup + every 5 minutes + on every battery change, but only actually **sent** if something differs from the last report; a reporting-toggle change forces a send unconditionally. `disabled` is a comma-separated group-name string, included on every actual send, absent if nothing's disabled |
| Phone → HA | `pebble_dashboard/report_status` command (device info) | `{id, type, status: {model, firmware, color}}` | Separate command with its own `id`; sent exactly once, at startup only; never combined with the periodic message above |
| Phone → HA | `pebble_dashboard/report_status` command (connection status) | `{id, type, status: {connected}}` | Phone-native, not relayed from the watch — see its own section above. Sent once per successful auth, then re-checked every 30s and sent only on change; reliability not yet confirmed on real hardware |
| HA → phone | `result` (reply to the command above) | `{id, type: "result", success: true}` | Ack only; phone ignores the reply content |

## Testing this contract without a real integration

The phone-side implementation (`src/pkjs/index.js`) has its own test suite at `test/ha_bridge.test.js` (mock WebSocket, no real HA server) that asserts on exactly the message shapes described above, including the initial-state-via-result path, a rejected/unknown-command scenario, the `report_status` relay (correct field naming, dropped when unauthenticated, nothing sent for an empty status), Clay's boolean toggle values converting to 1/0 ints for the `REPORT_ENABLE_*` keys, a string-valued field (`disabled`) relaying correctly alongside numeric ones, `on_color`/`off_color`/`hide_when` relaying correctly (including that each is independently optional), and the phone-native connection-status check (sent on auth, deduped on unchanged polls, not lost if the first check happens before authentication completes) — useful as a live reference for the wire format if this document and the code ever drift.
