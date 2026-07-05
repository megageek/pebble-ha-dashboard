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
];

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
    var value = settings[key];
    // Clay's "toggle" component (REPORT_ENABLE_*) reads a checkbox's raw
    // .checked property, i.e. a JS boolean, not the 0/1 int the C side and
    // AppMessage expect.
    watchSettings[key] = typeof value === "boolean" ? (value ? 1 : 0) : value;
  });

  Pebble.sendAppMessage(
    watchSettings,
    function () {
      console.log("Config sent to watch");
    },
    function (err) {
      console.log("Failed to send config: " + JSON.stringify(err));
    }
  );

  connectToHomeAssistant();
});

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
var NUM_HA_CHANNELS = 3;
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

// onColor/offColor/hideWhen are optional — see HA_INTEGRATION_SPEC.md.
// Their presence isn't what marks a channel as dot-capable (any channel
// can be assigned into the dot group on the watch side); they just style
// it if it is. hideWhen must be "none"/"on"/"off" (see parse_hide_when()
// on the C side).
function sendHaChannelToWatch(channel, value, label, onColor, offColor, hideWhen) {
  var channelNum = Number(channel);
  if (!(channelNum >= 1 && channelNum <= NUM_HA_CHANNELS)) {
    console.log("Ignoring HA update for unknown channel: " + channel);
    return;
  }

  var dict = {};
  dict["HA" + channelNum + "_VALUE"] = String(value).substring(0, 15);
  if (label !== undefined && label !== null) {
    dict["HA" + channelNum + "_LABEL"] = String(label).substring(0, 7);
  }
  if (onColor !== undefined && onColor !== null) {
    dict["HA" + channelNum + "_ON_COLOR"] = String(onColor).substring(0, 7);
  }
  if (offColor !== undefined && offColor !== null) {
    dict["HA" + channelNum + "_OFF_COLOR"] = String(offColor).substring(0, 7);
  }
  if (hideWhen !== undefined && hideWhen !== null) {
    dict["HA" + channelNum + "_HIDE_WHEN"] = String(hideWhen).substring(0, 4);
  }

  Pebble.sendAppMessage(
    dict,
    function () {
      console.log("Sent HA channel " + channelNum + " to watch");
    },
    function (err) {
      console.log("Failed to send HA channel " + channelNum + ": " + JSON.stringify(err));
    }
  );
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
  if (!config.url || !config.token) {
    console.log("Home Assistant not configured yet; not connecting");
    return;
  }

  if (haSocket) {
    haSocket.onclose = null; // this socket is being replaced, not lost
    haSocket.close();
  }
  haSubscriptionId = null;
  haAuthenticated = false;

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
        sendHaChannelToWatch(
          item.channel, item.value, item.label,
          item.on_color, item.off_color, item.hide_when
        );
      });
    } else if (message.id === haSubscriptionId && message.type === "event") {
      var item = message.event || {};
      sendHaChannelToWatch(
        item.channel, item.value, item.label,
        item.on_color, item.off_color, item.hide_when
      );
    }
  };

  haSocket.onerror = function (err) {
    console.log("Home Assistant socket error: " + JSON.stringify(err));
  };

  haSocket.onclose = function () {
    console.log("Home Assistant socket closed");
    haAuthenticated = false;
    scheduleHaReconnect();
  };
}

Pebble.addEventListener("ready", function () {
  connectToHomeAssistant();
});
