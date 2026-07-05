# Home Assistant Integration Spec

**This is the contract the Pebble watchface's phone-side bridge (`src/pkjs/index.js`) expects from Home Assistant.** It exists to be copied into whatever repo builds the actual HA-side custom integration — that side isn't implemented anywhere in *this* repo, only consumed. If you change the wire protocol here (command name, channel count, payload shape, truncation limits), update this file in the same commit; it's the only place the contract is written down independent of the watch/phone source.

*Last synced with `pebble-ha-dashboard` commit `42c23b8` + the custom-WS-command redesign (2026-07-05).*

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
  "label": "Temp"
}
```

- **`channel`** — integer `1`, `2`, or `3`. There are exactly **3 channels** in the current watchface (`CHANNEL_HA_1`/`CHANNEL_HA_2`/`CHANNEL_HA_3` on the watch side). Any other value (0, 4+, non-numeric) is silently ignored by the phone — not an error, just dropped.
- **`value`** — required. Always treated as a **plain string** by the watch today; there is no numeric/unit-aware or binary (on/off) channel type on the HA side yet, even though the watch's internal channel model supports those kinds for local channels (battery, steps). A temperature should be sent as a fully-formatted string like `"21.5°C"`, not a bare number — the watch will display exactly what's sent, with no unit suffix or number formatting applied.
- **`label`** — optional. If omitted, the watch keeps showing whatever label it last had for that channel (including the compiled-in default, e.g. `"HA1"`, if none has ever been sent). Only send it when it changes or on the first update for a channel.
- **Truncation, enforced phone-side, not HA-side**: `value` is cut to **15 characters**, `label` to **7 characters** before being relayed to the watch (hard limit of the watch's fixed-size buffers). Truncation is a dumb substring cut, not word-aware — prefer sending values that are already short (e.g. `"21.5°C"` rather than `"21.5 degrees Celsius"`) rather than relying on the phone to truncate sensibly.

## What's *not* in this contract

- **No numeric or binary channel kinds from HA yet.** Everything arrives and displays as text. If a future watchface version wants a real progress-bar-style HA channel (like battery today) or an ON/OFF-styled one (like a switch entity), that needs new message keys and C-side handling — this spec covers only what exists now.
- **No entity-to-channel mapping convention is prescribed here.** How a specific HA entity gets assigned to channel 1 vs 2 vs 3 (a config flow option, YAML, a dashboard helper, whatever) is entirely up to the integration's design — the watch and phone only know about channel numbers 1–3, never entity IDs.
- **No explicit unsubscribe command from the phone.** The phone never sends one; it just closes/replaces the connection (e.g. on reconnect or when Home Assistant config is re-saved). The integration should treat connection close as the unsubscribe signal, per HA's standard subscription cleanup pattern, rather than expecting an explicit unsubscribe message.

## Quick reference

| Direction | Message | Shape | Notes |
|---|---|---|---|
| Phone → HA | `pebble_dashboard/subscribe_channels` command | `{id, type}` | Sent once per successful auth (startup + every reconnect) |
| HA → phone | `result` (reply to the command above) | `{id, type: "result", success, result: {channels: [{channel, value, label?}, ...]}}` | Current state, sent once, immediately |
| HA → phone | `event` (tagged with the same `id`) | `{id, type: "event", event: {channel, value, label?}}` | Every subsequent change, ongoing |

## Testing this contract without a real integration

The phone-side implementation (`src/pkjs/index.js`) has its own test suite at `test/ha_bridge.test.js` (mock WebSocket, no real HA server) that asserts on exactly the message shapes described above, including the initial-state-via-result path and a rejected/unknown-command scenario — useful as a live reference for the wire format if this document and the code ever drift.
