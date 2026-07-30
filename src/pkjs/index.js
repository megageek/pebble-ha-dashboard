var Clay = require("pebble-clay");
var clayConfig = require("./config");
// autoHandleEvents: false — we register our own showConfiguration/webviewclosed
// listeners below so we can filter HA_URL/HA_TOKEN out of what gets sent to
// the watch (Clay's default handler would forward every setting as-is).
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

// Keys that actually go to the watch. HA_URL/HA_TOKEN live only in this
// phone's localStorage (via Clay) and are used below to open the Home
// Assistant WebSocket — never sent over AppMessage.
var WATCH_SETTINGS_KEYS = [
  "TEMPLATE_INDEX",
  "SLOT_0_CHANNEL",
  "SLOT_1_CHANNEL",
  "SLOT_2_CHANNEL",
  "SLOT_3_CHANNEL",
  "SLOT_4_CHANNEL",
  "SLOT_5_CHANNEL",
  "SLOT_6_CHANNEL",
  "SLOT_7_CHANNEL",
  "SLOT_8_CHANNEL",
  "SLOT_9_CHANNEL",
  "REPORT_ENABLE_BATTERY",
  "REPORT_ENABLE_STEPS",
  "REPORT_ENABLE_ACTIVITY",
  "REPORT_ENABLE_SLEEP",
  "REPORT_ENABLE_HEART_RATE",
  "REPORT_ENABLE_CONNECTED",
  "REPORT_ENABLE_DEVICE_INFO",
  "DOT_GROUP_SLOT",
  "DOT_0_CHANNEL",
  "DOT_1_CHANNEL",
  "DOT_2_CHANNEL",
  "DOT_3_CHANNEL",
  "DOT_SIZE",
  "CHARGE_ON_COLOR",
  "CHARGE_OFF_COLOR",
  "CHARGE_HIDE_WHEN",
  "CONN_ON_COLOR",
  "CONN_OFF_COLOR",
  "CONN_HIDE_WHEN",
  "TIME_BG_COLOR",
  "TIME_VALUE_COLOR",
  "TIME_LABEL_COLOR",
  "DATE_BG_COLOR",
  "DATE_VALUE_COLOR",
  "DATE_LABEL_COLOR",
  "BATTERY_BG_COLOR",
  "BATTERY_VALUE_COLOR",
  "BATTERY_LABEL_COLOR",
  "STEPS_BG_COLOR",
  "STEPS_VALUE_COLOR",
  "STEPS_LABEL_COLOR",
  "CHARGE_BG_COLOR",
  "CHARGE_VALUE_COLOR",
  "CHARGE_LABEL_COLOR",
  "CONN_BG_COLOR",
  "CONN_VALUE_COLOR",
  "CONN_LABEL_COLOR",
  "ACTIVE_MINUTES_BG_COLOR",
  "ACTIVE_MINUTES_VALUE_COLOR",
  "ACTIVE_MINUTES_LABEL_COLOR",
  "DISTANCE_BG_COLOR",
  "DISTANCE_VALUE_COLOR",
  "DISTANCE_LABEL_COLOR",
  "ACTIVE_KCAL_BG_COLOR",
  "ACTIVE_KCAL_VALUE_COLOR",
  "ACTIVE_KCAL_LABEL_COLOR",
  "RESTING_KCAL_BG_COLOR",
  "RESTING_KCAL_VALUE_COLOR",
  "RESTING_KCAL_LABEL_COLOR",
  "SLEEP_MINUTES_BG_COLOR",
  "SLEEP_MINUTES_VALUE_COLOR",
  "SLEEP_MINUTES_LABEL_COLOR",
  "SLEEP_RESTFUL_MINUTES_BG_COLOR",
  "SLEEP_RESTFUL_MINUTES_VALUE_COLOR",
  "SLEEP_RESTFUL_MINUTES_LABEL_COLOR",
  "HEART_RATE_BG_COLOR",
  "HEART_RATE_VALUE_COLOR",
  "HEART_RATE_LABEL_COLOR",
];

// Clay's "select" component always returns its value as a JS string — an
// HTML <option value="..."> is inherently string-typed, regardless of
// whether config.js declared the option's `value` as a number. The C side
// reads these fields with dict_find(...)->value->int32 with no type check
// (main.c's inbox_received_callback()), so a value that actually arrived
// as a CSTRING tuple (because it was a JS string here) gets its raw byte
// buffer reinterpreted as an int32 — garbage, not the intended number, and
// on real hardware this was implicated in webviewclosed-triggered
// AppMessage sends failing outright. A known-working reference watchface
// (peerdavid/pebble-mesh) explicitly parseInt()s every one of its own
// numeric select values before sending; this is that same fix, applied to
// every integer-typed select field here. Color/hide-when selects are
// deliberately excluded — those are meant to stay strings.
var NUMERIC_SETTINGS_KEYS = {
  TEMPLATE_INDEX: true,
  DOT_GROUP_SLOT: true,
  DOT_SIZE: true,
};
for (var numSlotIdx = 0; numSlotIdx < 10; numSlotIdx++) {
  NUMERIC_SETTINGS_KEYS["SLOT_" + numSlotIdx + "_CHANNEL"] = true;
}
for (var numDotIdx = 0; numDotIdx < 4; numDotIdx++) {
  NUMERIC_SETTINGS_KEYS["DOT_" + numDotIdx + "_CHANNEL"] = true;
}

function coerceWatchSettingValue(key, value) {
  // Clay's "toggle" component (REPORT_ENABLE_*) reads a checkbox's raw
  // .checked property, i.e. a JS boolean, not the 0/1 int the C side and
  // AppMessage expect.
  if (typeof value === "boolean") {
    return value ? 1 : 0;
  }
  if (NUMERIC_SETTINGS_KEYS[key]) {
    return Number(value);
  }
  return value;
}

Pebble.addEventListener("showConfiguration", function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener("webviewclosed", function (e) {
  if (!e || !e.response) {
    return;
  }

  // clay.getSettings() also persists everything (incl. HA_URL/HA_TOKEN) to
  // localStorage as a side effect, which is what connectToHomeAssistant()
  // reads from. Pass `false` for raw, string-keyed settings (matching the
  // messageKey names in config.js) — the default (`true`) converts keys to
  // their numeric AppMessage IDs, which would break the name-based lookup
  // below. Pebble.sendAppMessage() resolves string keys against
  // package.json's messageKeys itself, so no manual conversion is needed.
  var settings = clay.getSettings(e.response, false);

  var watchSettings = {};
  WATCH_SETTINGS_KEYS.forEach(function (key) {
    if (settings[key] === undefined) {
      return;
    }
    watchSettings[key] = coerceWatchSettingValue(key, settings[key]);
  });

  // Previously deferred via setTimeout, on the theory that Android could
  // close the Clay WebView before PebbleKit had restored the app-message
  // bridge used by this callback. Real-device testing disproved that: every
  // config save failed outright regardless of message size, chunking,
  // retries, or HA WebSocket overlap (all ruled out separately) — and a
  // known-working reference watchface (peerdavid/pebble-mesh) sends its own
  // config synchronously, with no defer at all, directly inside its
  // webviewclosed handler, and does so reliably. Dispatching synchronously
  // here too, matching that working pattern.
  sendWatchSettings(watchSettings, connectToHomeAssistant);

  // A config save is a fresh user-initiated attempt — allow a failure to
  // notify again even if the previous URL/token already triggered one.
  haFailureNotified = false;
});

// Pebble.sendAppMessage() can fail with "Failed to connect to Pebble" for
// reasons outside this code's control (observed on a real Android phone
// most often in the first few seconds after the Pebble app itself
// (re)starts, before its own Bluetooth session to the watch has finished
// warming up). Retrying after a short delay, the same pattern already used
// for the HA WebSocket connection (see scheduleHaReconnect below), recovers
// from that transient window instead of dropping the send forever.
var APP_MESSAGE_RETRY_DELAY_MS = 3000;
var APP_MESSAGE_MAX_ATTEMPTS = 5;
var APP_MESSAGE_CALLBACK_TIMEOUT_MS = 10000;
var appMessageQueue = [];
var appMessageSending = false;

function pumpAppMessageQueue() {
  if (appMessageSending || appMessageQueue.length === 0) return;
  appMessageSending = true;
  var entry = appMessageQueue[0];

  function finish() {
    appMessageQueue.shift();
    appMessageSending = false;
    pumpAppMessageQueue();
    // Fires whether this entry succeeded or gave up after exhausting
    // retries — unlike onSuccess, which only fires on success. Lets a
    // caller know delivery is *done* (not necessarily successful) so it can
    // safely start other AppMessage/network activity without overlapping
    // this entry's own retries.
    if (entry.onSettled) entry.onSettled();
  }

  function attemptSend() {
    entry.attempt++;
    var settled = false;
    var callbackTimer;

    function failed(err) {
      if (settled) return;
      settled = true;
      clearTimeout(callbackTimer);
      console.log("Failed to send " + entry.description + " (attempt " + entry.attempt + "): " + JSON.stringify(err));
      if (entry.attempt >= APP_MESSAGE_MAX_ATTEMPTS) {
        console.log("Giving up sending " + entry.description + " after " + entry.attempt + " attempts");
        finish();
        return;
      }
      setTimeout(attemptSend, APP_MESSAGE_RETRY_DELAY_MS);
    }

    callbackTimer = setTimeout(function () {
      failed({ error: "Pebble.sendAppMessage callback timeout" });
    }, APP_MESSAGE_CALLBACK_TIMEOUT_MS);

    Pebble.sendAppMessage(entry.dict, function () {
      if (settled) return;
      settled = true;
      clearTimeout(callbackTimer);
      if (entry.onSuccess) entry.onSuccess();
      finish();
    }, failed);
  }

  attemptSend();
}

// PebbleKit AppMessage has one in-flight transaction. Clay config chunks
// and HA updates must therefore share one global queue.
function sendAppMessageWithRetry(dict, description, onSuccess, onSettled) {
  appMessageQueue.push({ dict: dict, description: description, onSuccess: onSuccess, onSettled: onSettled, attempt: 0 });
  pumpAppMessageQueue();
}

// The full config surface (WATCH_SETTINGS_KEYS) is 69 keys — 10 slots, 7
// report toggles, dot config, and 39 per-channel color fields. Splitting
// into smaller chunks sent strictly sequentially (each waiting for the
// previous chunk's ack before the next is sent) keeps any one message
// modest in size, on top of the retry behavior above.
var CONFIG_CHUNK_SIZE = 15;

// onComplete (if given) fires once every chunk has settled — the last
// chunk either succeeded or gave up after exhausting retries — so a caller
// can sequence other AppMessage/network activity (see the HA reconnect
// deferral in webviewclosed above) to start only once this burst is
// entirely finished, not while it's still in flight.
function sendWatchSettings(watchSettings, onComplete) {
  var keys = Object.keys(watchSettings);
  if (keys.length === 0) {
    console.log("Config sent to watch");
    if (onComplete) onComplete();
    return;
  }

  for (var index = 0; index < keys.length; index += CONFIG_CHUNK_SIZE) {
    var chunk = {};
    keys.slice(index, index + CONFIG_CHUNK_SIZE).forEach(function (key) {
      chunk[key] = watchSettings[key];
    });
    var isLastChunk = index + CONFIG_CHUNK_SIZE >= keys.length;
    sendAppMessageWithRetry(
      chunk,
      "config chunk",
      isLastChunk ? function () { console.log("Config sent to watch"); } : null,
      isLastChunk ? onComplete : null
    );
  }
}

// ---------------------------------------------------------------------------
// Home Assistant WebSocket bridge
//
// pkjs holds a persistent WebSocket to HA's native /api/websocket endpoint
// (same connection/auth session HA already provides — not a separate
// server) and issues one custom command, registered by the HA-side
// integration itself, to both fetch current channel state and subscribe to
// future changes in a single round trip. There is no polling and no
// generic-event-bus usage — see HA_INTEGRATION_SPEC.md for the full
// contract this expects from the integration.
// ---------------------------------------------------------------------------

// A custom WebSocket API command (see websocket_api.async_register_command
// in the HA integration), not a core command — it isn't admin-gated unless
// the integration explicitly adds that, unlike fire_event.
var HA_SUBSCRIBE_COMMAND = "pebble_dashboard/subscribe_channels";
var NUM_HA_CHANNELS = 10;
var RECONNECT_BASE_DELAY_MS = 2000;
var RECONNECT_MAX_DELAY_MS = 30000;

var haSocket = null;
var haReconnectTimer = null;
var haReconnectDelay = RECONNECT_BASE_DELAY_MS;
var haNextRequestId = 1;
// The id of our subscribe_channels command, so incoming "result"/"event"
// messages can be matched to it (HA multiplexes everything over one
// connection, keyed by request id).
var haSubscriptionId = null;
// True only between auth_ok and the connection being replaced/dropped —
// gates outbound status reports so they're never sent to a half-open socket.
var haAuthenticated = false;
// Set the first time a connection attempt fails (auth_invalid, or a close
// without ever reaching auth_ok) since the last user-initiated attempt
// (app launch or a Clay config save). Without this, a bad URL/token would
// renotify on every automatic reconnect forever, since scheduleHaReconnect()
// retries indefinitely.
var haFailureNotified = false;
// Sticky per-channel "kind" cache (phone-side). The watch's ch->kind is
// already sticky (see apply_ha_channel_updates() in main.c); HA is allowed
// to send `kind` once and omit it on every later value-only update (see
// HA_INTEGRATION_SPEC.md's `kind` field). Without this cache,
// sendHaChannelToWatch() would have no memory of a channel's kind and
// would re-encode `value` as a string on a value-only follow-up, which
// makes the C side's unconditional `ch->kind = CHANNEL_KIND_TEXT` (on any
// TUPLE_CSTRING HAn_VALUE) silently revert the channel to text rendering.
// Indexed by channel number (1-10). Deliberately NOT reset on reconnect —
// see connectToHomeAssistant() below.
var haChannelKindCache = {};

function getHaConfig() {
  var settings = {};
  try {
    settings = JSON.parse(localStorage.getItem("clay-settings")) || {};
  } catch (e) {
    console.log("Failed to read Home Assistant config: " + e.toString());
  }
  return {
    url: settings.HA_URL || "",
    token: settings.HA_TOKEN || "",
  };
}

// Surfaces a connection/auth problem as a watch notification, since pkjs has
// no UI of its own and console.log is otherwise invisible outside `pebble
// logs`. Only fires once per haFailureNotified cycle — see its declaration
// above for why repeats are suppressed.
function notifyHaConnectionFailure(message) {
  if (haFailureNotified) {
    return;
  }
  haFailureNotified = true;
  if (typeof Pebble.showSimpleNotificationOnPebble === "function") {
    Pebble.showSimpleNotificationOnPebble("HA Dashboard", message);
  }
}

function scheduleHaReconnect() {
  if (haReconnectTimer) {
    return;
  }
  console.log("Reconnecting to Home Assistant in " + haReconnectDelay + "ms");
  haReconnectTimer = setTimeout(function () {
    haReconnectTimer = null;
    connectToHomeAssistant();
  }, haReconnectDelay);
  haReconnectDelay = Math.min(haReconnectDelay * 2, RECONNECT_MAX_DELAY_MS);
}

// Every field except channel/value is optional — see HA_INTEGRATION_SPEC.md.
// Takes the raw HA payload object (same shape whether it came from the
// initial subscribe result or a later event) rather than a long positional
// argument list, since that shape already exists at both call sites and
// keeps growing as more optional styling/typing fields are added.
//
// item.kind: "text" (default)/"numeric"/"binary" — decides whether
// item.value is sent as a string or a number; sticky (see
// haChannelKindCache above and parse_channel_kind() on the C side), so it
// only needs to be sent once, not on every update, though sending it every
// time is harmless.
// item.min/item.max only take effect together (see main.c) — sent alone,
// either is silently ignored.
// item.hide_when must be "none"/"on"/"off" (see parse_hide_when()).
// item.style must be "raw"/"bar" (see parse_channel_style()).
//
// Pulled out of sendHaChannelToWatch() below so the per-channel throttle can
// build the dict lazily, from whichever item (leading or coalesced-pending)
// is actually being relayed at the time.
function buildHaChannelDict(item, channelNum) {
  var dict = {};
  // See haChannelKindCache above: fall back to the last-known kind for
  // this channel when this message omits it, rather than assuming "text".
  if (item.kind !== undefined && item.kind !== null) {
    haChannelKindCache[channelNum] = item.kind;
  }
  var effectiveKind = item.kind !== undefined && item.kind !== null ? item.kind : haChannelKindCache[channelNum];
  var isNumeric = effectiveKind === "numeric" || effectiveKind === "binary";
  dict["HA" + channelNum + "_VALUE"] = isNumeric ? Number(item.value) : String(item.value).substring(0, 15);
  if (item.label !== undefined && item.label !== null) {
    dict["HA" + channelNum + "_LABEL"] = String(item.label).substring(0, 7);
  }
  if (item.kind !== undefined && item.kind !== null) {
    dict["HA" + channelNum + "_KIND"] = String(item.kind).substring(0, 7);
  }
  if (item.unit !== undefined && item.unit !== null) {
    dict["HA" + channelNum + "_UNIT"] = String(item.unit).substring(0, 7);
  }
  if (item.min !== undefined && item.min !== null && item.max !== undefined && item.max !== null) {
    dict["HA" + channelNum + "_MIN"] = Number(item.min);
    dict["HA" + channelNum + "_MAX"] = Number(item.max);
  }
  if (item.style !== undefined && item.style !== null) {
    dict["HA" + channelNum + "_STYLE"] = String(item.style).substring(0, 3);
  }
  if (item.on_color !== undefined && item.on_color !== null) {
    dict["HA" + channelNum + "_ON_COLOR"] = String(item.on_color).substring(0, 7);
  }
  if (item.off_color !== undefined && item.off_color !== null) {
    dict["HA" + channelNum + "_OFF_COLOR"] = String(item.off_color).substring(0, 7);
  }
  if (item.hide_when !== undefined && item.hide_when !== null) {
    dict["HA" + channelNum + "_HIDE_WHEN"] = String(item.hide_when).substring(0, 4);
  }
  if (item.bg_color !== undefined && item.bg_color !== null) {
    dict["HA" + channelNum + "_BG_COLOR"] = String(item.bg_color).substring(0, 7);
  }
  if (item.value_color !== undefined && item.value_color !== null) {
    dict["HA" + channelNum + "_VALUE_COLOR"] = String(item.value_color).substring(0, 7);
  }
  if (item.label_color !== undefined && item.label_color !== null) {
    dict["HA" + channelNum + "_LABEL_COLOR"] = String(item.label_color).substring(0, 7);
  }
  return dict;
}

// Every AppMessage transaction wakes the watch's Bluetooth radio — a chatty
// HA entity (a power meter, a temperature sensor updating every second or
// two) relayed with no throttling here would keep that radio from ever
// sleeping and drain the watch battery fast, since nothing on the HA side
// or in HA_INTEGRATION_SPEC.md promises a minimum update interval.
//
// Leading-edge/trailing-edge throttle, independent per channel: the first
// update to a quiet channel relays immediately (so the watch face still
// feels live), then a HA_CHANNEL_THROTTLE_MS window opens during which
// further updates to that same channel are coalesced — only the latest is
// kept — and relayed once more, at the window's end, if anything arrived.
// A single slow-but-steady channel is therefore unaffected (each update
// lands after the previous window closes and relays immediately again); a
// channel updating many times per second collapses to at most one
// AppMessage every HA_CHANNEL_THROTTLE_MS instead of one per event.
var HA_CHANNEL_THROTTLE_MS = 5000;
var haChannelThrottleTimer = {}; // channelNum -> timer while a window is open
var haChannelPendingItem = {}; // channelNum -> latest item queued during an open window

// Sticky per-channel dedupe cache: the exact dict (JSON-serialized) last
// actually relayed for this channel. Some HA integrations re-announce
// unchanged state; relaying that again would cost another AppMessage/BT
// wake for no visible change. Deliberately NOT reset on reconnect, same
// reasoning as haChannelKindCache above — HA resending identical state
// after a reconnect doesn't need re-relaying either.
var haChannelLastSentDict = {};

function relayHaChannelDict(channelNum, dict) {
  var key = JSON.stringify(dict);
  if (haChannelLastSentDict[channelNum] === key) {
    return; // identical to what's already on the watch; not worth a BT wake
  }
  haChannelLastSentDict[channelNum] = key;
  sendAppMessageWithRetry(dict, "HA channel " + channelNum, function () {
    console.log("Sent HA channel " + channelNum + " to watch");
  });
}

function sendHaChannelToWatch(item) {
  var channelNum = Number(item.channel);
  if (!(channelNum >= 1 && channelNum <= NUM_HA_CHANNELS)) {
    console.log("Ignoring HA update for unknown channel: " + item.channel);
    return;
  }

  if (haChannelThrottleTimer[channelNum]) {
    // A window is already open for this channel — remember the latest item
    // and let the window's trailing edge relay it, rather than sending now.
    haChannelPendingItem[channelNum] = item;
    return;
  }

  relayHaChannelDict(channelNum, buildHaChannelDict(item, channelNum));
  haChannelThrottleTimer[channelNum] = setTimeout(function () {
    haChannelThrottleTimer[channelNum] = null;
    var pending = haChannelPendingItem[channelNum];
    haChannelPendingItem[channelNum] = null;
    if (pending) {
      relayHaChannelDict(channelNum, buildHaChannelDict(pending, channelNum));
    }
  }, HA_CHANNEL_THROTTLE_MS);
}

function reportStatusToHomeAssistant(payload) {
  if (!haAuthenticated || !haSocket) {
    return; // no connection yet; the next periodic report from the watch will retry
  }

  var status = {};
  Object.keys(payload || {}).forEach(function (key) {
    if (key.indexOf("REPORT_") === 0) {
      status[key.substring("REPORT_".length).toLowerCase()] = payload[key];
    }
  });

  if (Object.keys(status).length === 0) {
    return;
  }

  haSocket.send(
    JSON.stringify({
      id: haNextRequestId++,
      type: "pebble_dashboard/report_status",
      status: status,
    })
  );
}

Pebble.addEventListener("appmessage", function (e) {
  reportStatusToHomeAssistant(e.payload);
});

function connectToHomeAssistant() {
  var config = getHaConfig();

  // Tear down any existing connection unconditionally, even if the new
  // config turns out to be unset below — otherwise clearing HA_URL/HA_TOKEN
  // in Clay would silently leave a stale connection (from the previous
  // config) running in the background instead of actually disconnecting.
  if (haSocket) {
    haSocket.onclose = null; // this socket is being replaced, not lost
    haSocket.close();
    haSocket = null;
  }
  haSubscriptionId = null;
  haAuthenticated = false;

  if (!config.url || !config.token) {
    console.log("Home Assistant not configured yet; not connecting");
    return;
  }
  // haChannelKindCache is deliberately NOT reset here — HA is expected to
  // resend full current state (including kind) via the initial `result`
  // after reconnect anyway, and keeping stale cached state around is
  // harmless and strictly better than forgetting it.

  var wsUrl = config.url.replace(/\/+$/, "") + "/api/websocket";
  console.log("Connecting to Home Assistant: " + wsUrl);
  haSocket = new WebSocket(wsUrl);

  haSocket.onmessage = function (event) {
    var message;
    try {
      message = JSON.parse(event.data);
    } catch (e) {
      console.log("Failed to parse Home Assistant message: " + e.toString());
      return;
    }

    if (message.type === "auth_required") {
      haSocket.send(JSON.stringify({ type: "auth", access_token: config.token }));
    } else if (message.type === "auth_ok") {
      console.log("Home Assistant auth succeeded");
      haReconnectDelay = RECONNECT_BASE_DELAY_MS;
      haAuthenticated = true;
      haSubscriptionId = haNextRequestId++;
      // One command both fetches current state (in the "result" reply
      // below) and subscribes to future changes ("event" messages tagged
      // with this same id) — no separate request-state round trip needed.
      haSocket.send(JSON.stringify({ id: haSubscriptionId, type: HA_SUBSCRIBE_COMMAND }));
    } else if (message.type === "auth_invalid") {
      console.log("Home Assistant auth failed: " + message.message);
      notifyHaConnectionFailure("Home Assistant login failed. Check HA_TOKEN in the watchapp settings.");
    } else if (message.id === haSubscriptionId && message.type === "result") {
      if (!message.success) {
        // Most likely cause: the HA-side integration isn't installed, so
        // this custom command isn't registered at all.
        console.log(
          "Home Assistant rejected " + HA_SUBSCRIBE_COMMAND + ": " + JSON.stringify(message.error)
        );
        return;
      }
      var channels = (message.result && message.result.channels) || [];
      channels.forEach(function (item) {
        sendHaChannelToWatch(item);
      });
    } else if (message.id === haSubscriptionId && message.type === "event") {
      sendHaChannelToWatch(message.event || {});
    }
  };

  haSocket.onerror = function (err) {
    console.log("Home Assistant socket error: " + JSON.stringify(err));
  };

  haSocket.onclose = function () {
    console.log("Home Assistant socket closed");
    if (!haAuthenticated) {
      // Never got as far as auth_ok this attempt — most likely an
      // unreachable/misconfigured HA_URL (auth_invalid, handled above,
      // already covers a reachable host that rejected the token).
      notifyHaConnectionFailure("Couldn't connect to Home Assistant. Check HA_URL in the watchapp settings.");
    }
    haAuthenticated = false;
    scheduleHaReconnect();
  };
}

Pebble.addEventListener("ready", function () {
  connectToHomeAssistant();
});
