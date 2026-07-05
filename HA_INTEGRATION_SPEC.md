# Home Assistant Integration Spec

**This is the contract the Pebble watchface's phone-side bridge (`src/pkjs/index.js`) expects from Home Assistant.** It exists to be copied into whatever repo builds the actual HA-side custom integration/automation — that side isn't implemented anywhere in *this* repo, only consumed. If you change the wire protocol here (event names, channel count, payload shape, truncation limits), update this file in the same commit; it's the only place the contract is written down independent of the watch/phone source.

*Last synced with `pebble-ha-dashboard` commit `aecd970` (2026-07-05).*

## Transport

Home Assistant's native WebSocket API, not REST and not a custom endpoint: `ws(s)://<host>:<port>/api/websocket`. The phone (pkjs) is the client; it never polls, it holds one persistent connection and reconnects with exponential backoff (2s → 30s, phone-side concern, nothing HA needs to do about it) if dropped.

Auth is a standard HA long-lived access token, sent through HA's normal WebSocket auth handshake:

1. Server → client: `{"type": "auth_required"}`
2. Client → server: `{"type": "auth", "access_token": "<token>"}`
3. Server → client: `{"type": "auth_ok"}` or `{"type": "auth_invalid", "message": "..."}`

**The token's user must be a Home Assistant administrator.** Nothing else in this contract needs elevated permissions except step 2 below (`fire_event` is admin-only in HA's WebSocket API).

## What the phone does after auth_ok

Every time auth succeeds — both the very first connection and every reconnect after a drop — the phone does two things, in order:

1. **Subscribes to events:**
   ```json
   {"id": <n>, "type": "subscribe_events", "event_type": "pebble_channel_update"}
   ```
2. **Requests current state:**
   ```json
   {"id": <n>, "type": "fire_event", "event_type": "pebble_request_state"}
   ```

Step 2 exists so the watch shows real data immediately on startup/reconnect instead of sitting on placeholder text until something happens to change on the HA side. It deliberately reuses the same event type as an organic push (see below) rather than being a distinct request/response message — from the phone's point of view, an initial-state event and a live-change event are indistinguishable.

## What Home Assistant must do

### 1. Fire `pebble_channel_update` whenever a channel's value changes

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

### 2. Fire `pebble_channel_update` for every managed channel in response to `pebble_request_state`

When the `pebble_request_state` event arrives (no payload to inspect — its arrival is the whole signal), re-publish the current value of every channel currently mapped to something, using the exact same `pebble_channel_update` event and payload shape as an organic change. If a channel isn't currently mapped to anything, it's fine to simply not fire an event for it — the watch keeps its placeholder.

There is no acknowledgement/response expected back from this — it's fire-and-forget in both directions, same as ordinary change-triggered updates.

## What's *not* in this contract

- **No numeric or binary channel kinds from HA yet.** Everything arrives and displays as text. If a future watchface version wants a real progress-bar-style HA channel (like battery today) or an ON/OFF-styled one (like a switch entity), that needs new message keys and C-side handling — this spec covers only what exists now.
- **No entity-to-channel mapping convention is prescribed here.** How a specific HA entity gets assigned to channel 1 vs 2 vs 3 (a config flow option, YAML, a dashboard helper, whatever) is entirely up to the integration/automation design — the watch and phone only know about channel numbers 1–3, never entity IDs.
- **No `get_states` usage.** Initial state is delivered via the `pebble_request_state`/`pebble_channel_update` event pair above, not HA's built-in `get_states` WebSocket command, so there's no requirement to expose channel values as queryable entity states (though nothing stops an implementation from *also* doing that for its own purposes).

## Quick reference

| Direction | Event type | Payload | Notes |
|---|---|---|---|
| HA → phone | `pebble_channel_update` | `{channel: 1-3, value: string, label?: string}` | Organic change, or response to a state request |
| Phone → HA | `pebble_request_state` | *(none)* | Fired once per successful auth (startup + every reconnect) |

## Testing this contract without a real integration

The phone-side implementation (`src/pkjs/index.js`) has its own test suite at `test/ha_bridge.test.js` (mock WebSocket, no real HA server) that asserts on exactly the message shapes described above — useful as a live reference for the wire format if this document and the code ever drift.
