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
  // reads from.
  var settings = clay.getSettings(e.response);

  var watchSettings = {};
  WATCH_SETTINGS_KEYS.forEach(function (key) {
    if (settings[key] !== undefined) {
      watchSettings[key] = settings[key];
    }
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
// Home Assistant pushes channel updates by firing a custom event (fired by a
// user-configured automation/integration action); pkjs holds a persistent
// WebSocket to HA's native /api/websocket endpoint and relays each event to
// the watch immediately via AppMessage. There is no polling and no
// watch-originated request — see CLAUDE.md for the full design.
// ---------------------------------------------------------------------------

var HA_EVENT_TYPE = "pebble_channel_update";
var NUM_HA_CHANNELS = 3;
var RECONNECT_BASE_DELAY_MS = 2000;
var RECONNECT_MAX_DELAY_MS = 30000;

var haSocket = null;
var haReconnectTimer = null;
var haReconnectDelay = RECONNECT_BASE_DELAY_MS;
var haNextRequestId = 1;

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

function sendHaChannelToWatch(channel, value, label) {
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
      haSocket.send(
        JSON.stringify({
          id: haNextRequestId++,
          type: "subscribe_events",
          event_type: HA_EVENT_TYPE,
        })
      );
    } else if (message.type === "auth_invalid") {
      console.log("Home Assistant auth failed: " + message.message);
    } else if (
      message.type === "event" &&
      message.event &&
      message.event.event_type === HA_EVENT_TYPE
    ) {
      var data = message.event.data || {};
      sendHaChannelToWatch(data.channel, data.value, data.label);
    }
  };

  haSocket.onerror = function (err) {
    console.log("Home Assistant socket error: " + JSON.stringify(err));
  };

  haSocket.onclose = function () {
    console.log("Home Assistant socket closed");
    scheduleHaReconnect();
  };
}

Pebble.addEventListener("ready", function () {
  connectToHomeAssistant();
});
