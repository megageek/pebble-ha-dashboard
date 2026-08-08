// Mock-WebSocket harness for src/pkjs/index.js's Home Assistant bridge.
//
// There is no real HA server or phone/webview available in this
// environment, so this stubs Pebble/localStorage/WebSocket/setTimeout as
// globals, loads the *built* pkjs bundle (run `pebble build` first), and
// scripts a mock socket through the auth handshake plus various channel
// update / reconnect scenarios — asserting on what the bridge sends to the
// mock socket and to the stubbed Pebble.sendAppMessage().
//
// Run with: node test/ha_bridge.test.js

var path = require("path");

var BUNDLE_PATH = path.join(__dirname, "..", "build", "pebble-js-app.js");

var HA_SUBSCRIBE_COMMAND = "pebble_dashboard/subscribe_channels"; // must match src/pkjs/index.js
var HA1_VALUE_LIMIT = 15; // must match ChannelData.text[16] on the C side
var LABEL_LIMIT = 7; // must match ChannelData.label[8] on the C side
// kind/style/hide_when are parsed into C enums (parse_channel_kind()/
// parse_channel_style()/parse_hide_when()), not copied into a fixed
// buffer, so these two limits are JS-side-only lengths sized to fit the
// longest legal value ("numeric"=7, "bar"/"raw"=3) rather than mirroring
// a ChannelData struct field.
var KIND_UNIT_LIMIT = 7; // must match sendHaChannelToWatch()'s kind/unit truncation
var STYLE_LIMIT = 3; // must match sendHaChannelToWatch()'s style truncation
var HIDE_WHEN_LIMIT = 4; // must match parse_hide_when()'s "none"/"on"/"off" vocabulary
// on_color/off_color/bg_color/value_color/label_color DO mirror real
// char[8] buffers on the C side (ChannelData.on_color etc.), same as
// LABEL_LIMIT above.
var STYLING_COLOR_LIMIT = 7;

var failures = 0;

// Real Clay WebView responses wrap every field as {value: X, ...}, not a
// flat value — clay.getSettings(response, false) just returns
// JSON.parse(response) verbatim (see node_modules/pebble-clay's own
// getSettings() source), so whatever shape is in the response string IS
// what index.js receives. A real-device bug (settings[key] used directly,
// never unwrapping .value) went undetected for a long time because the
// flat objects previously used here for fake responses skip the exact
// unwrapping step real Clay output requires. Every fake webviewclosed
// response in this file must go through this helper, not raw
// JSON.stringify(), to actually exercise that unwrapping path.
function clayResponse(flatSettings) {
  var wrapped = {};
  Object.keys(flatSettings).forEach(function (key) {
    wrapped[key] = { value: flatSettings[key] };
  });
  return JSON.stringify(wrapped);
}

function assert(cond, msg) {
  if (!cond) {
    failures++;
    console.error("FAIL: " + msg);
  } else {
    console.log("OK: " + msg);
  }
}

function freshEnv() {
  var listeners = {};
  var sentAppMessages = [];
  // HA_CONN_VALUE (see sendHaConnectionStatusToWatch() in src/pkjs/index.js)
  // is pushed automatically on every auth_ok/onclose, which every test
  // using env.authenticate() triggers — kept out of sentAppMessages so the
  // many pre-existing tests that index/count sentAppMessages positionally
  // (channel relays, config chunks, etc.) don't have to account for it.
  var haConnAppMessages = [];
  var scheduledTimeouts = [];
  var scheduledIntervals = [];
  var lastSocket = null;
  var notifications = [];

  global.localStorage = {
    store: {
      "clay-settings": JSON.stringify({
        HA_URL: "http://ha.local:8123",
        HA_TOKEN: "testtoken",
      }),
    },
    getItem: function (k) {
      return this.store[k] || null;
    },
    setItem: function (k, v) {
      this.store[k] = v;
    },
  };

  global.Pebble = {
    platform: "basalt",
    addEventListener: function (name, cb) {
      listeners[name] = listeners[name] || [];
      listeners[name].push(cb);
    },
    openURL: function () {},
    getActiveWatchInfo: function () {
      return null;
    },
    getAccountToken: function () {
      return "";
    },
    getWatchToken: function () {
      return "";
    },
    sendAppMessage: function (dict, onSuccess) {
      if ("HA_CONN_VALUE" in dict) {
        haConnAppMessages.push(dict);
      } else {
        sentAppMessages.push(dict);
      }
      if (onSuccess) onSuccess();
    },
    showSimpleNotificationOnPebble: function (title, body) {
      notifications.push({ title: title, body: body });
    },
  };

  function MockWebSocket(url) {
    this.url = url;
    this.sent = [];
    lastSocket = this;
  }
  MockWebSocket.prototype.send = function (data) {
    this.sent.push(JSON.parse(data));
  };
  MockWebSocket.prototype.close = function () {
    this.closed = true;
  };
  global.WebSocket = MockWebSocket;

  global.setTimeout = function (cb, delay) {
    var timer = { cb: cb, delay: delay };
    scheduledTimeouts.push(timer);
    return timer;
  };
  global.clearTimeout = function (timer) {
    var index = scheduledTimeouts.indexOf(timer);
    if (index !== -1) scheduledTimeouts.splice(index, 1);
  };
  // Never fires automatically — a real setInterval would keep the real
  // Node process alive across every test (each freshEnv() re-requires the
  // bundle, so a real interval would also stack up new intervals on every
  // test). Tests that care about periodic behavior fire it manually via
  // env.scheduledIntervals.
  global.setInterval = function (cb, delay) {
    var timer = { cb: cb, delay: delay };
    scheduledIntervals.push(timer);
    return timer;
  };
  global.clearInterval = function (timer) {
    var index = scheduledIntervals.indexOf(timer);
    if (index !== -1) scheduledIntervals.splice(index, 1);
  };

  // Each test needs its own copy of the bridge's module-level state
  // (haSocket, haReconnectDelay, etc.), so bust the require cache.
  delete require.cache[require.resolve(BUNDLE_PATH)];
  require(BUNDLE_PATH);

  function fire(name, arg) {
    (listeners[name] || []).forEach(function (cb) {
      cb(arg);
    });
  }

  function authenticate() {
    fire("ready");
    lastSocket.onmessage({ data: JSON.stringify({ type: "auth_required" }) });
    lastSocket.onmessage({ data: JSON.stringify({ type: "auth_ok" }) });
  }

  function subscriptionId(socket) {
    for (var i = (socket || lastSocket).sent.length - 1; i >= 0; i--) {
      if ((socket || lastSocket).sent[i].type === HA_SUBSCRIBE_COMMAND) {
        return (socket || lastSocket).sent[i].id;
      }
    }
    return null;
  }

  // Takes the raw event payload object directly (channel/value/label/
  // on_color/off_color/hide_when/bg_color/value_color/label_color/kind/
  // unit/min/max/style, all optional except channel/value) rather than a
  // long positional list — mirrors sendHaChannelToWatch()'s own item-object
  // shape in src/pkjs/index.js, and means omitting a field is just omitting
  // a key instead of threading `undefined` through earlier positions.
  function sendChannelEvent(data) {
    lastSocket.onmessage({
      data: JSON.stringify({ id: subscriptionId(), type: "event", event: data }),
    });
  }

  function sendInitialResult(channels) {
    lastSocket.onmessage({
      data: JSON.stringify({
        id: subscriptionId(),
        type: "result",
        success: true,
        result: { channels: channels },
      }),
    });
  }

  function sendAppMessageFromWatch(payload) {
    fire("appmessage", { payload: payload });
  }

  function sendRejectedResult(error) {
    lastSocket.onmessage({
      data: JSON.stringify({
        id: subscriptionId(),
        type: "result",
        success: false,
        error: error || { code: "unknown_command" },
      }),
    });
  }

  return {
    fire: fire,
    authenticate: authenticate,
    subscriptionId: subscriptionId,
    sendChannelEvent: sendChannelEvent,
    sendInitialResult: sendInitialResult,
    sendRejectedResult: sendRejectedResult,
    sendAppMessageFromWatch: sendAppMessageFromWatch,
    sentAppMessages: sentAppMessages,
    haConnAppMessages: haConnAppMessages,
    scheduledTimeouts: scheduledTimeouts,
    scheduledIntervals: scheduledIntervals,
    notifications: notifications,
    getLastSocket: function () {
      return lastSocket;
    },
  };
}

// --- Test 1: startup handshake sequencing (auth -> single subscribe command) ---
(function testStartupSequencing() {
  var env = freshEnv();
  env.fire("ready");
  var socket = env.getLastSocket();

  assert(socket.url === "http://ha.local:8123/api/websocket", "connects to the configured HA URL");

  socket.onmessage({ data: JSON.stringify({ type: "auth_required" }) });
  assert(
    socket.sent[0].type === "auth" && socket.sent[0].access_token === "testtoken",
    "responds to auth_required with the configured token"
  );
  assert(socket.sent.length === 1, "sends nothing else before auth_ok, got " + socket.sent.length);

  socket.onmessage({ data: JSON.stringify({ type: "auth_ok" }) });
  // Two more: the subscribe_channels command, then a phone-native
  // connection-status report_status (see checkWatchConnection() in
  // src/pkjs/index.js) — sent right after subscribing, not relayed from
  // the watch's own AppMessage report.
  assert(socket.sent.length === 3, "sends two more messages after auth_ok, got " + socket.sent.length);
  assert(
    socket.sent[1].type === HA_SUBSCRIBE_COMMAND && typeof socket.sent[1].id === "number",
    "sends the subscribe_channels command with a request id first, got " + JSON.stringify(socket.sent[1])
  );
  assert(
    socket.sent[2].type === "pebble_dashboard/report_status" && socket.sent[2].status.connected === 0,
    "sends a phone-native connection-status report right after subscribing, got " + JSON.stringify(socket.sent[2])
  );
})();

// --- Test 1b: connection status is only re-sent when it actually changes,
// not on every periodic poll (see checkWatchConnection() / setInterval in
// src/pkjs/index.js) — the whole point of this being phone-native is to
// cost nothing extra, so a steady-state watch must not generate a message
// every WATCH_CONNECTION_POLL_MS regardless of state. ---
(function testWatchConnectionDedupedWhenUnchanged() {
  var env = freshEnv();
  env.authenticate(); // getActiveWatchInfo mock returns null -> connected:0 sent once
  var socket = env.getLastSocket();
  var sentBefore = socket.sent.length;

  var pollTimer = env.scheduledIntervals[0];
  assert(pollTimer, "a periodic connection-check interval was scheduled");
  pollTimer.cb();
  pollTimer.cb();
  pollTimer.cb();

  assert(
    socket.sent.length === sentBefore,
    "repeated polls with no actual change send nothing new, got " + (socket.sent.length - sentBefore) + " new message(s)"
  );
})();

// --- Test 1c: a real change in watch connectivity, picked up on the next
// poll, is reported — and only once, not every subsequent poll ---
(function testWatchConnectionChangeDetectedViaPoll() {
  var env = freshEnv();
  env.authenticate();
  var socket = env.getLastSocket();

  global.Pebble.getActiveWatchInfo = function () {
    return { platform: "emery" }; // now "connected"
  };

  var pollTimer = env.scheduledIntervals[0];
  pollTimer.cb();

  var reportMsg = socket.sent[socket.sent.length - 1];
  assert(
    reportMsg.type === "pebble_dashboard/report_status" && reportMsg.status.connected === 1,
    "a real connectivity change is reported, got " + JSON.stringify(reportMsg)
  );

  var sentAfterChange = socket.sent.length;
  pollTimer.cb();
  assert(socket.sent.length === sentAfterChange, "the same (still-connected) state polled again sends nothing new");
})();

// --- Test 1d: if the very first connection check happens before HA is
// authenticated (e.g. fired from `ready`, before the WebSocket handshake
// finishes), the state isn't wrongly recorded as "already reported" —
// regression test for a bug where haLastKnownWatchConnected would be set
// even though sendStatusToHomeAssistant() silently dropped the send. ---
(function testWatchConnectionNotLostWhenUnauthenticated() {
  var env = freshEnv();
  env.fire("ready"); // socket opens, but not yet authenticated
  var pollTimer = env.scheduledIntervals[0];
  assert(pollTimer, "a periodic connection-check interval was scheduled from ready");

  pollTimer.cb(); // fires while unauthenticated; must be silently dropped, not recorded as sent
  var socket = env.getLastSocket();
  assert(socket.sent.length === 0, "nothing sent to HA before authentication completes");

  socket.onmessage({ data: JSON.stringify({ type: "auth_required" }) });
  socket.onmessage({ data: JSON.stringify({ type: "auth_ok" }) });

  var reportMsg = socket.sent[socket.sent.length - 1];
  assert(
    reportMsg.type === "pebble_dashboard/report_status" && reportMsg.status.connected === 0,
    "connection status is still reported once authenticated, not lost from the earlier dropped attempt, got " +
      JSON.stringify(reportMsg)
  );
})();

// --- Test 2: initial state arrives via the subscribe command's "result" reply ---
(function testInitialStateViaResult() {
  var env = freshEnv();
  env.authenticate();

  env.sendInitialResult([
    { channel: 1, value: "72F", label: "Temp" },
    { channel: 2, value: "Closed" }, // no label — channel keeps its default
    { channel: 11, value: "ignored" }, // out of range, must not relay
  ]);

  assert(env.sentAppMessages.length === 2, "relayed 2 AppMessages from the initial result, got " + env.sentAppMessages.length);
  assert(
    env.sentAppMessages[0].HA1_VALUE === "72F" && env.sentAppMessages[0].HA1_LABEL === "Temp",
    "channel 1 initial state relayed correctly, got " + JSON.stringify(env.sentAppMessages[0])
  );
  assert(
    env.sentAppMessages[1].HA2_VALUE === "Closed" && !("HA2_LABEL" in env.sentAppMessages[1]),
    "channel 2 initial state relayed without a label, got " + JSON.stringify(env.sentAppMessages[1])
  );
})();

// --- Test 3: a rejected subscribe command (integration not installed) doesn't crash or relay ---
(function testRejectedSubscribeCommand() {
  var env = freshEnv();
  env.authenticate();

  env.sendRejectedResult({ code: "unknown_command", message: "Unknown command." });

  assert(env.sentAppMessages.length === 0, "no AppMessage sent when the subscribe command is rejected");
})();

// --- Test 4: each of the 3 channels relays independently via push events, value+label ---
(function testAllThreeChannelsRelay() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 1, value: "72F", label: "Temp" });
  env.sendChannelEvent({ channel: 2, value: "Open", label: "Door" });
  env.sendChannelEvent({ channel: 3, value: "On", label: "Light" });

  assert(env.sentAppMessages.length === 3, "relayed 3 separate AppMessages, got " + env.sentAppMessages.length);
  assert(
    env.sentAppMessages[0].HA1_VALUE === "72F" && env.sentAppMessages[0].HA1_LABEL === "Temp",
    "channel 1 relayed correctly, got " + JSON.stringify(env.sentAppMessages[0])
  );
  assert(
    env.sentAppMessages[1].HA2_VALUE === "Open" && env.sentAppMessages[1].HA2_LABEL === "Door",
    "channel 2 relayed correctly, got " + JSON.stringify(env.sentAppMessages[1])
  );
  assert(
    env.sentAppMessages[2].HA3_VALUE === "On" && env.sentAppMessages[2].HA3_LABEL === "Light",
    "channel 3 relayed correctly, got " + JSON.stringify(env.sentAppMessages[2])
  );
})();

// --- Test 5: value-only push event omits the label key entirely ---
(function testValueOnlyUpdate() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 2, value: "73F" }); // no label passed

  assert(env.sentAppMessages.length === 1, "relayed one AppMessage for a value-only update");
  var msg = env.sentAppMessages[0];
  assert(msg.HA2_VALUE === "73F", "value present, got " + JSON.stringify(msg));
  assert(!("HA2_LABEL" in msg), "label key omitted entirely, got " + JSON.stringify(msg));
})();

// --- Test 6: long value/label get truncated to the C-side buffer sizes ---
(function testTruncation() {
  var env = freshEnv();
  env.authenticate();

  var longValue = "123456789012345678901234567890"; // 30 chars
  var longLabel = "A Very Long Sensor Label"; // 24 chars
  env.sendChannelEvent({ channel: 1, value: longValue, label: longLabel });

  var msg = env.sentAppMessages[0];
  assert(
    msg.HA1_VALUE.length === HA1_VALUE_LIMIT && msg.HA1_VALUE === longValue.substring(0, HA1_VALUE_LIMIT),
    "value truncated to " + HA1_VALUE_LIMIT + " chars, got " + JSON.stringify(msg.HA1_VALUE)
  );
  assert(
    msg.HA1_LABEL.length === LABEL_LIMIT && msg.HA1_LABEL === longLabel.substring(0, LABEL_LIMIT),
    "label truncated to " + LABEL_LIMIT + " chars, got " + JSON.stringify(msg.HA1_LABEL)
  );
})();

// --- Test 6b: HA-supplied dot styling (on_color/off_color/hide_when)
// relays through to the watch as HAn_ON_COLOR/HAn_OFF_COLOR/HAn_HIDE_WHEN ---
(function testDotStylingRelay() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 2, value: "on", label: "Door", on_color: "red", off_color: "green", hide_when: "off" });

  assert(env.sentAppMessages.length === 1, "relayed one AppMessage for a dot-styled update");
  var msg = env.sentAppMessages[0];
  assert(
    msg.HA2_ON_COLOR === "red" && msg.HA2_OFF_COLOR === "green" && msg.HA2_HIDE_WHEN === "off",
    "on_color/off_color/hide_when relayed correctly, got " + JSON.stringify(msg)
  );
})();

// --- Test 6c: dot styling fields are each independently optional ---
(function testDotStylingFieldsOptional() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 3, value: "off", on_color: "blue" }); // only on_color, no label/off_color/hide_when

  var msg = env.sentAppMessages[0];
  assert(
    msg.HA3_ON_COLOR === "blue" &&
      !("HA3_LABEL" in msg) &&
      !("HA3_OFF_COLOR" in msg) &&
      !("HA3_HIDE_WHEN" in msg),
    "only the supplied dot-styling field is sent, got " + JSON.stringify(msg)
  );
})();

// --- Test 6d: general-purpose bg_color/value_color/label_color relay
// through to the watch as HAn_BG_COLOR/HAn_VALUE_COLOR/HAn_LABEL_COLOR,
// independently of the dot on_color/off_color/hide_when fields above ---
(function testChannelStylingColorsRelay() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({
    channel: 1, value: "21.5C", label: "Temp",
    bg_color: "purple", value_color: "yellow", label_color: "blue",
  });

  var msg = env.sentAppMessages[env.sentAppMessages.length - 1];
  assert(
    msg.HA1_BG_COLOR === "purple" && msg.HA1_VALUE_COLOR === "yellow" && msg.HA1_LABEL_COLOR === "blue",
    "bg_color/value_color/label_color relayed correctly, got " + JSON.stringify(msg)
  );
  assert(
    !("HA1_ON_COLOR" in msg) && !("HA1_OFF_COLOR" in msg) && !("HA1_HIDE_WHEN" in msg),
    "dot on/off/hide-when fields stay independently optional, got " + JSON.stringify(msg)
  );
})();

// --- Test 6e: styling/kind/unit/style fields are each truncated to their
// documented limits (see HA_INTEGRATION_SPEC.md's Truncation bullet),
// mirroring testTruncation's value/label coverage above ---
(function testStylingFieldsTruncation() {
  var env = freshEnv();
  env.authenticate();

  var longColor = "extremelylongcolorname"; // > 7 chars
  var longHideWhen = "always"; // > 4 chars
  var longKind = "numericish"; // > 7 chars
  var longUnit = "kilometers"; // > 7 chars
  var longStyle = "barish"; // > 3 chars

  env.sendChannelEvent({
    channel: 5,
    value: 10,
    kind: longKind,
    unit: longUnit,
    style: longStyle,
    on_color: longColor,
    off_color: longColor,
    hide_when: longHideWhen,
    bg_color: longColor,
    value_color: longColor,
    label_color: longColor,
  });

  var msg = env.sentAppMessages[0];
  assert(
    msg.HA5_KIND === longKind.substring(0, KIND_UNIT_LIMIT) && msg.HA5_KIND.length === KIND_UNIT_LIMIT,
    "kind truncated to " + KIND_UNIT_LIMIT + " chars, got " + JSON.stringify(msg.HA5_KIND)
  );
  assert(
    msg.HA5_UNIT === longUnit.substring(0, KIND_UNIT_LIMIT) && msg.HA5_UNIT.length === KIND_UNIT_LIMIT,
    "unit truncated to " + KIND_UNIT_LIMIT + " chars, got " + JSON.stringify(msg.HA5_UNIT)
  );
  assert(
    msg.HA5_STYLE === longStyle.substring(0, STYLE_LIMIT) && msg.HA5_STYLE.length === STYLE_LIMIT,
    "style truncated to " + STYLE_LIMIT + " chars, got " + JSON.stringify(msg.HA5_STYLE)
  );
  assert(
    msg.HA5_HIDE_WHEN === longHideWhen.substring(0, HIDE_WHEN_LIMIT) && msg.HA5_HIDE_WHEN.length === HIDE_WHEN_LIMIT,
    "hide_when truncated to " + HIDE_WHEN_LIMIT + " chars, got " + JSON.stringify(msg.HA5_HIDE_WHEN)
  );
  ["ON_COLOR", "OFF_COLOR", "BG_COLOR", "VALUE_COLOR", "LABEL_COLOR"].forEach(function (suffix) {
    var key = "HA5_" + suffix;
    assert(
      msg[key] === longColor.substring(0, STYLING_COLOR_LIMIT) && msg[key].length === STYLING_COLOR_LIMIT,
      key + " truncated to " + STYLING_COLOR_LIMIT + " chars, got " + JSON.stringify(msg[key])
    );
  });
})();

// --- Test 7: unknown channel numbers are ignored, not relayed ---
(function testUnknownChannelIgnored() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 0, value: "x", label: "y" });
  env.sendChannelEvent({ channel: 11, value: "x", label: "y" });
  env.sendChannelEvent({ channel: "not-a-number", value: "x", label: "y" });

  assert(env.sentAppMessages.length === 0, "no AppMessage sent for out-of-range channels, got " + env.sentAppMessages.length);
})();

// --- Test 7b: channels 4-10 (the boosted range) relay just as channels
// 1-3 always have, via the same generic "HA" + channelNum key building ---
(function testExtendedChannelRangeRelays() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 4, value: "on", label: "Fan" });
  env.sendChannelEvent({ channel: 10, value: "68F", label: "Attic" });

  assert(env.sentAppMessages.length === 2, "relayed both extended-range channels, got " + env.sentAppMessages.length);
  assert(
    env.sentAppMessages[0].HA4_VALUE === "on" && env.sentAppMessages[0].HA4_LABEL === "Fan",
    "channel 4 relayed correctly, got " + JSON.stringify(env.sentAppMessages[0])
  );
  assert(
    env.sentAppMessages[1].HA10_VALUE === "68F" && env.sentAppMessages[1].HA10_LABEL === "Attic",
    "channel 10 (the new upper bound) relayed correctly, got " + JSON.stringify(env.sentAppMessages[1])
  );
})();

// --- Test 7c: kind="numeric" sends HAn_VALUE as a number (not a string)
// plus HAn_KIND/HAn_UNIT relaying ---
(function testNumericChannelRelay() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 1, value: 21, label: "Temp", kind: "numeric", unit: "C" });

  var msg = env.sentAppMessages[0];
  assert(
    msg.HA1_VALUE === 21 && typeof msg.HA1_VALUE === "number",
    "numeric value sent as a number, got " + JSON.stringify(msg.HA1_VALUE) + " (" + typeof msg.HA1_VALUE + ")"
  );
  assert(msg.HA1_KIND === "numeric" && msg.HA1_UNIT === "C", "kind/unit relayed correctly, got " + JSON.stringify(msg));
})();

// --- Test 7d: kind="binary" also sends HAn_VALUE as a number ---
(function testBinaryChannelRelay() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 2, value: 1, label: "Door", kind: "binary" });

  var msg = env.sentAppMessages[0];
  assert(
    msg.HA2_VALUE === 1 && typeof msg.HA2_VALUE === "number" && msg.HA2_KIND === "binary",
    "binary value sent as a number with kind relayed, got " + JSON.stringify(msg)
  );
})();

// --- Test 7d-2: kind is sticky across a later value-only update — a
// channel that received kind:"numeric" once must still encode value as a
// number on a later message that omits kind entirely (regression test for
// the phone-side haChannelKindCache in sendHaChannelToWatch()) ---
(function testStickyKindAcrossValueOnlyUpdate() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 1, value: 21, kind: "numeric" });
  // Arrives while channel 1's throttle window is still open (see the
  // per-channel throttle in sendHaChannelToWatch) — coalesced, not sent yet.
  env.sendChannelEvent({ channel: 1, value: 22 }); // no kind this time
  assert(env.sentAppMessages.length === 1, "the second update is coalesced, not sent immediately");

  var throttleTimer = env.scheduledTimeouts.filter(function (t) { return t.delay === 5000; })[0];
  assert(throttleTimer, "a throttle window is open for channel 1");
  throttleTimer.cb(); // trailing edge: relays the latest coalesced item

  assert(env.sentAppMessages.length === 2, "the trailing edge relays the coalesced update, got " + env.sentAppMessages.length);
  var msg = env.sentAppMessages[env.sentAppMessages.length - 1];
  assert(
    msg.HA1_VALUE === 22 && typeof msg.HA1_VALUE === "number",
    "value-only update after a numeric kind still encodes as a number, got " +
      JSON.stringify(msg.HA1_VALUE) + " (" + typeof msg.HA1_VALUE + ")"
  );
  assert(!("HA1_KIND" in msg), "kind key omitted on the value-only follow-up, got " + JSON.stringify(msg));
})();

// --- Test 7d-4: a burst of rapid updates to the same channel coalesces to
// exactly 2 AppMessages (leading edge + trailing edge with the latest
// value), not one per event — this is the actual battery fix: an HA entity
// updating many times within the throttle window must not wake the watch's
// Bluetooth radio once per event ---
(function testChannelBurstCoalescesToTrailingEdge() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 1, value: 20 });
  env.sendChannelEvent({ channel: 1, value: 21 });
  env.sendChannelEvent({ channel: 1, value: 22 });
  env.sendChannelEvent({ channel: 1, value: 23 });

  assert(env.sentAppMessages.length === 1, "only the leading-edge update is sent immediately, got " + env.sentAppMessages.length);

  var throttleTimer = env.scheduledTimeouts.filter(function (t) { return t.delay === 5000; })[0];
  throttleTimer.cb();

  assert(env.sentAppMessages.length === 2, "the burst coalesces to exactly 2 AppMessages total, got " + env.sentAppMessages.length);
  assert(
    env.sentAppMessages[0].HA1_VALUE === "20" && env.sentAppMessages[1].HA1_VALUE === "23",
    "leading edge carries the first value and trailing edge carries the latest, got " +
      JSON.stringify([env.sentAppMessages[0].HA1_VALUE, env.sentAppMessages[1].HA1_VALUE])
  );
})();

// --- Test 7d-5: after a throttle window closes, a channel re-announcing
// the exact same state it last relayed is deduped rather than resent ---
(function testIdenticalRepeatIsDeduped() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 1, value: "72F", label: "Temp" });
  assert(env.sentAppMessages.length === 1, "first update relays");

  var throttleTimer = env.scheduledTimeouts.filter(function (t) { return t.delay === 5000; })[0];
  throttleTimer.cb(); // close the window with nothing pending

  env.sendChannelEvent({ channel: 1, value: "72F", label: "Temp" }); // identical repeat
  assert(env.sentAppMessages.length === 1, "identical repeat is deduped, not resent, got " + env.sentAppMessages.length);

  // The deduped repeat still opened its own throttle window (see
  // sendHaChannelToWatch — leading-edge processing always opens one,
  // whether or not the dedupe check actually sent anything); close it
  // before sending a genuine change so that change gets its own leading edge.
  env.scheduledTimeouts.filter(function (t) { return t.delay === 5000; }).forEach(function (t) { t.cb(); });

  env.sendChannelEvent({ channel: 1, value: "73F", label: "Temp" }); // an actual change
  assert(env.sentAppMessages.length === 2, "a genuine change after a deduped repeat still relays, got " + env.sentAppMessages.length);
})();

// --- Test 7d-3: the sticky-kind cache is per-channel — a value-only
// update on a channel that never received kind still encodes as a string,
// even though a *different* channel's kind was cached as numeric ---
(function testStickyKindCacheIsPerChannel() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 1, value: 21, kind: "numeric" });
  env.sendChannelEvent({ channel: 2, value: 22 }); // different channel, never sent kind

  var msg = env.sentAppMessages[env.sentAppMessages.length - 1];
  assert(
    msg.HA2_VALUE === "22" && typeof msg.HA2_VALUE === "string",
    "a different channel's value-only update is unaffected by another channel's cached kind, got " +
      JSON.stringify(msg.HA2_VALUE) + " (" + typeof msg.HA2_VALUE + ")"
  );
})();

// --- Test 7e: min/max/style relay together for a ranged numeric channel ---
(function testRangeAndStyleRelay() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 3, value: 45, kind: "numeric", min: 0, max: 100, style: "bar" });

  var msg = env.sentAppMessages[0];
  assert(
    msg.HA3_MIN === 0 && msg.HA3_MAX === 100 && msg.HA3_STYLE === "bar",
    "min/max/style relayed correctly, got " + JSON.stringify(msg)
  );
})();

// --- Test 7f: min sent without max is dropped entirely, not sent half-set ---
(function testMinWithoutMaxDropped() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent({ channel: 4, value: 10, kind: "numeric", min: 0 });

  var msg = env.sentAppMessages[0];
  assert(
    !("HA4_MIN" in msg) && !("HA4_MAX" in msg),
    "min without max is not relayed, got " + JSON.stringify(msg)
  );
})();

// --- Test 8: reconnect backoff doubles on repeated drops, resets after auth_ok ---
(function testReconnectBackoff() {
  var env = freshEnv();
  env.authenticate(); // haReconnectDelay reset to 2000 here

  var socket1 = env.getLastSocket();
  socket1.onclose();
  assert(env.scheduledTimeouts.length === 1, "scheduled a reconnect after first drop");
  assert(env.scheduledTimeouts[0].delay === 2000, "first reconnect delay is base (2000ms), got " + env.scheduledTimeouts[0].delay);

  // Fire the scheduled reconnect without authenticating — simulates a
  // connection that opens but drops again before auth_ok.
  env.scheduledTimeouts[0].cb();
  var socket2 = env.getLastSocket();
  assert(socket2 !== socket1, "a new socket was created on reconnect");
  socket2.onclose();
  assert(env.scheduledTimeouts.length === 2, "scheduled a second reconnect after second drop");
  assert(env.scheduledTimeouts[1].delay === 4000, "second reconnect delay doubled (4000ms), got " + env.scheduledTimeouts[1].delay);

  // This time let it succeed — delay should reset back to base.
  env.scheduledTimeouts[1].cb();
  var socket3 = env.getLastSocket();
  socket3.onmessage({ data: JSON.stringify({ type: "auth_required" }) });
  socket3.onmessage({ data: JSON.stringify({ type: "auth_ok" }) });
  socket3.onclose();
  assert(env.scheduledTimeouts.length === 3, "scheduled a third reconnect after third drop");
  assert(
    env.scheduledTimeouts[2].delay === 2000,
    "reconnect delay reset to base (2000ms) after a successful auth, got " + env.scheduledTimeouts[2].delay
  );
})();

// --- Test 8b: auth_invalid (bad HA_TOKEN) on a `ready`-triggered (not
// save-triggered) connection attempt never notifies at all — only a
// connection attempt caused directly by a Clay config save may notify
// (see haConnectionAttemptIsSaveTriggered in src/pkjs/index.js) ---
(function testAuthInvalidOnReadyDoesNotNotify() {
  var env = freshEnv();
  env.fire("ready");
  var socket1 = env.getLastSocket();
  socket1.onmessage({ data: JSON.stringify({ type: "auth_required" }) });
  socket1.onmessage({ data: JSON.stringify({ type: "auth_invalid", message: "Invalid access token" }) });
  socket1.onclose();

  assert(env.notifications.length === 0, "a ready-triggered auth_invalid does not notify, got " + env.notifications.length);

  // Automatic reconnect retries with the same bad token must not notify either.
  env.scheduledTimeouts[0].cb();
  var socket2 = env.getLastSocket();
  socket2.onmessage({ data: JSON.stringify({ type: "auth_required" }) });
  socket2.onmessage({ data: JSON.stringify({ type: "auth_invalid", message: "Invalid access token" }) });
  socket2.onclose();

  assert(env.notifications.length === 0, "repeated auth_invalid on auto-reconnect still does not notify");
})();

// --- Test 8c: a connection that never reaches auth_ok (bad/unreachable
// HA_URL), also `ready`-triggered, never notifies either ---
(function testUnreachableHostOnReadyDoesNotNotify() {
  var env = freshEnv();
  env.fire("ready");
  env.getLastSocket().onclose(); // dropped before auth_required/auth_ok ever arrived

  assert(env.notifications.length === 0, "a ready-triggered unreachable host does not notify, got " + env.notifications.length);

  env.scheduledTimeouts[0].cb();
  env.getLastSocket().onclose();
  assert(env.notifications.length === 0, "repeated failure on auto-reconnect still does not notify");
})();

// --- Test 8d: only a connection attempt directly triggered by a Clay
// config save may notify, and it does so exactly once even though a
// `ready`-triggered failure happened first and stayed silent ---
(function testOnlyConfigSaveTriggeredFailureNotifies() {
  var env = freshEnv();
  env.fire("ready");
  env.getLastSocket().onclose();
  assert(env.notifications.length === 0, "ready-triggered failure notifies nothing");

  env.fire(
    "webviewclosed",
    { response: clayResponse({ HA_URL: "http://still-bad.local:8123", HA_TOKEN: "t" }) }
  );
  env.getLastSocket().onclose();
  assert(env.notifications.length === 1, "a config-save-triggered failure notifies once, got " + env.notifications.length);
  assert(
    /couldn't connect|could not connect/i.test(env.notifications[0].body),
    "generic connection-failure message, got " + JSON.stringify(env.notifications[0])
  );
})();

// --- Test 8d-2: the actual crux of issue #1 — a config-save-triggered
// failure notifies once, but the automatic reconnect(s) that follow it
// must not notify again, even though they're retrying the very same bad
// config that already failed once ---
(function testAutomaticReconnectAfterSaveDoesNotRenotify() {
  var env = freshEnv();
  env.fire(
    "webviewclosed",
    { response: clayResponse({ HA_URL: "http://bad.local:8123", HA_TOKEN: "t" }) }
  );
  env.getLastSocket().onclose(); // save-triggered failure: notifies once
  assert(env.notifications.length === 1, "save-triggered failure notifies once, got " + env.notifications.length);

  env.scheduledTimeouts[env.scheduledTimeouts.length - 1].cb(); // automatic reconnect
  env.getLastSocket().onclose(); // fails again, automatically
  assert(env.notifications.length === 1, "an automatic reconnect failure after a save does not renotify, got " + env.notifications.length);
})();

// --- Test 8e: a normal drop after a successful connection does not
// spuriously fire the generic failure notification ---
(function testSuccessfulDropDoesNotNotify() {
  var env = freshEnv();
  env.authenticate();
  env.getLastSocket().onclose();

  assert(env.notifications.length === 0, "dropping an already-authenticated connection notifies nothing, got " + env.notifications.length);
})();

// --- Test 8e-2: clearing HA_URL/HA_TOKEN via a config save actually closes
// the previously-open socket instead of leaving it running in the
// background — regression test for a bug where connectToHomeAssistant()'s
// early return (config unset) skipped tearing down the old connection ---
(function testClearingConfigClosesExistingSocket() {
  var env = freshEnv();
  env.authenticate();
  var oldSocket = env.getLastSocket();

  env.fire("webviewclosed", { response: clayResponse({ HA_URL: "", HA_TOKEN: "" }) });

  assert(oldSocket.closed === true, "the previously-open socket is closed when config is cleared");
  assert(env.getLastSocket() === oldSocket, "no new socket is opened for an unset config");
})();

// --- Test 8e-3/4/5: the phone's own live connection state TO Home
// Assistant (haAuthenticated) is pushed to the watch as HA_CONN_VALUE
// whenever it changes — see CHANNEL_HA_CONNECTED in src/c/types.h and
// sendHaConnectionStatusToWatch() in src/pkjs/index.js. A different signal
// from checkWatchConnection()'s watch<->phone Bluetooth report (Tests 1/
// 1b/1c/1d above), which is sent to HA over the WebSocket, not to the
// watch over AppMessage. ---
(function testHaConnValuePushedOnAuthOk() {
  var env = freshEnv();
  env.authenticate();

  assert(env.haConnAppMessages.length === 1, "HA_CONN_VALUE pushed once after auth_ok, got " + env.haConnAppMessages.length);
  assert(env.haConnAppMessages[0].HA_CONN_VALUE === 1, "pushed as 1 (connected), got " + JSON.stringify(env.haConnAppMessages[0]));
})();

(function testHaConnValuePushedOnClose() {
  var env = freshEnv();
  env.authenticate();
  env.getLastSocket().onclose();

  assert(env.haConnAppMessages.length === 2, "HA_CONN_VALUE pushed again on close, got " + env.haConnAppMessages.length);
  assert(env.haConnAppMessages[1].HA_CONN_VALUE === 0, "pushed as 0 (disconnected) on close, got " + JSON.stringify(env.haConnAppMessages[1]));
})();

(function testHaConnValueNotResentWhenUnchanged() {
  var env = freshEnv();
  env.authenticate();
  // A second auth_ok on the same socket (not a realistic HA sequence, but
  // exercises the dedup path directly) must not push HA_CONN_VALUE again,
  // since the state (connected) hasn't actually changed.
  env.getLastSocket().onmessage({ data: JSON.stringify({ type: "auth_ok" }) });

  assert(env.haConnAppMessages.length === 1, "unchanged state is not re-pushed, got " + env.haConnAppMessages.length);
})();

// --- Test 8f: a failed AppMessage send is retried after a delay instead
// of being dropped forever, and succeeds once the retry goes through.
// Retries exist for genuinely transient conditions — see
// APP_MESSAGE_RETRY_DELAY_MS's comment in src/pkjs/index.js for why that's
// not a license to assume every failure is transient/external. ---
(function testFailedSendRetriesAndSucceeds() {
  var env = freshEnv();
  env.authenticate();

  var attempts = 0;
  var delivered = null;
  global.Pebble.sendAppMessage = function (dict, onSuccess, onError) {
    attempts++;
    if (attempts === 1) {
      onError({ error: "Failed to connect to Pebble" });
    } else {
      delivered = dict;
      onSuccess();
    }
  };

  env.sendChannelEvent({ channel: 1, value: "72F", label: "Temp" });

  assert(attempts === 1, "first attempt happens immediately, got " + attempts);
  // Two timers are live at this point: the 3000ms send retry, and the
  // 5000ms per-channel throttle window opened by the leading-edge relay
  // attempt (see sendHaChannelToWatch's throttle in src/pkjs/index.js).
  var retryTimer = env.scheduledTimeouts.filter(function (t) { return t.delay === 3000; })[0];
  assert(retryTimer, "a retry is scheduled after a failed send, got delays " + env.scheduledTimeouts.map(function (t) { return t.delay; }));

  retryTimer.cb();

  assert(attempts === 2, "retry actually re-attempts the send, got " + attempts);
  assert(
    delivered && delivered.HA1_VALUE === "72F",
    "the retried send delivers the original data, got " + JSON.stringify(delivered)
  );
})();

// --- Test 8g: an AppMessage send that keeps failing gives up after a
// bounded number of attempts instead of retrying forever ---
(function testFailedSendGivesUpAfterMaxAttempts() {
  var env = freshEnv();
  env.authenticate();

  var attempts = 0;
  global.Pebble.sendAppMessage = function (dict, onSuccess, onError) {
    attempts++;
    onError({ error: "Failed to connect to Pebble" });
  };

  env.sendChannelEvent({ channel: 1, value: "72F" });
  while (env.scheduledTimeouts.length > 0) {
    env.scheduledTimeouts.shift().cb();
  }

  assert(attempts === 5, "gives up after 5 total attempts, got " + attempts);
  assert(env.scheduledTimeouts.length === 0, "no further retry scheduled after the last attempt");
})();

// --- Test 8h: an HA snapshot is globally serialized; forEach must not
// create concurrent PebbleKit AppMessage transactions. ---
(function testHaSnapshotUsesSingleFlightQueue() {
  var env = freshEnv();
  env.authenticate();
  var sends = [];
  var callbacks = [];
  global.Pebble.sendAppMessage = function (dict, onSuccess) {
    sends.push(dict);
    callbacks.push(onSuccess);
  };

  env.sendInitialResult([
    { channel: 1, value: "one" },
    { channel: 2, value: "two" },
    { channel: 3, value: "three" },
  ]);
  assert(sends.length === 1, "HA snapshot starts only one AppMessage before its ack, got " + sends.length);
  callbacks[0]();
  assert(sends.length === 2, "first ack releases exactly the second HA update, got " + sends.length);
  callbacks[1]();
  assert(sends.length === 3, "second ack releases exactly the third HA update, got " + sends.length);
})();

// --- Test 9: webviewclosed filters HA_URL/HA_TOKEN out of the AppMessage
// sent to the watch, but still reconnects using the new values ---
(function testWebviewClosedFiltersAndReconnects() {
  var env = freshEnv();
  env.fire("ready");
  var firstSocket = env.getLastSocket();
  assert(firstSocket.url === "http://ha.local:8123/api/websocket", "initial connect uses the original URL");

  var claySavePayload = clayResponse({
    TEMPLATE_INDEX: 1,
    SLOT_0_CHANNEL: 4,
    SLOT_1_CHANNEL: 1,
    SLOT_2_CHANNEL: 2,
    SLOT_3_CHANNEL: 3,
    SLOT_4_CHANNEL: 0,
    REPORT_ENABLE_BATTERY: true,
    REPORT_ENABLE_HEART_RATE: false, // Clay's toggle component -> raw JS boolean
    DOT_GROUP_SLOT: 2,
    DOT_0_CHANNEL: 7,
    DOT_SIZE: 0,
    HA_URL: "http://new-ha.local:8123",
    HA_TOKEN: "newtoken",
  });

  env.fire("webviewclosed", { response: claySavePayload });

  // Dispatched synchronously, inside webviewclosed itself — matching a
  // known-working reference watchface (peerdavid/pebble-mesh), which sends
  // its own config the same way with no defer. An earlier setTimeout-based
  // defer (0ms, then 3000ms) was real-device-tested and made no difference;
  // every config save still failed outright regardless of the delay.
  assert(env.sentAppMessages.length === 1, "config save sent exactly one AppMessage to the watch");
  var watchMsg = env.sentAppMessages[0];
  assert(watchMsg.TEMPLATE_INDEX === 1, "TEMPLATE_INDEX forwarded to the watch, got " + JSON.stringify(watchMsg));
  assert(watchMsg.SLOT_0_CHANNEL === 4, "SLOT_0_CHANNEL forwarded to the watch");
  assert(!("HA_URL" in watchMsg) && !("HA_TOKEN" in watchMsg), "HA_URL/HA_TOKEN withheld from the watch, got " + JSON.stringify(watchMsg));
  assert(
    watchMsg.REPORT_ENABLE_BATTERY === 1 && watchMsg.REPORT_ENABLE_HEART_RATE === 0,
    "Clay's boolean toggle values converted to 1/0 ints, got " +
      JSON.stringify({ b: watchMsg.REPORT_ENABLE_BATTERY, hr: watchMsg.REPORT_ENABLE_HEART_RATE })
  );
  assert(
    watchMsg.DOT_GROUP_SLOT === 2 && watchMsg.DOT_0_CHANNEL === 7 && watchMsg.DOT_SIZE === 0,
    "dot-group config forwarded to the watch, got " +
      JSON.stringify({ slot: watchMsg.DOT_GROUP_SLOT, ch: watchMsg.DOT_0_CHANNEL, size: watchMsg.DOT_SIZE })
  );

  var secondSocket = env.getLastSocket();
  assert(
    secondSocket !== firstSocket && secondSocket.url === "http://new-ha.local:8123/api/websocket",
    "webviewclosed reconnected using the newly saved URL, got " + secondSocket.url
  );
})();

// --- Test 9a-2: Clay's "select" component always returns its value as a JS
// string (an HTML <option value="..."> is inherently string-typed) — real
// Clay saves never send TEMPLATE_INDEX/SLOT_n_CHANNEL/DOT_* as JS numbers,
// even though config.js declares their option values as numbers. main.c
// reads these fields with dict_find(...)->value->int32 with no type check,
// so a string-typed tuple there produces garbage, not the intended number.
// This is a real bug found via real-device testing (a known-working
// reference watchface, peerdavid/pebble-mesh, explicitly parseInt()s every
// one of its own numeric select values before sending) — this test uses
// string values for these fields, matching Clay's actual behavior, unlike
// every other test above which builds its fake e.response with JS number
// literals and would never have caught this. ---
(function testNumericSelectValuesCoercedFromClayStrings() {
  var env = freshEnv();
  env.fire("ready");

  // clayResponse() wraps each of these in {value: X}, matching Clay's real
  // wire format; the string values themselves match what a "select"
  // component actually produces, regardless of how config.js declared the
  // option's `value`.
  var claySavePayload = clayResponse({
    TEMPLATE_INDEX: "1",
    SLOT_0_CHANNEL: "4",
    DOT_GROUP_SLOT: "2",
    DOT_0_CHANNEL: "7",
    DOT_SIZE: "0",
    CHARGE_ON_COLOR: "red", // color/hide-when selects are also strings, but must STAY strings
  });

  env.fire("webviewclosed", { response: claySavePayload });

  var watchMsg = env.sentAppMessages[0];
  assert(
    watchMsg.TEMPLATE_INDEX === 1 && typeof watchMsg.TEMPLATE_INDEX === "number",
    "TEMPLATE_INDEX coerced from Clay's string to a real number, got " +
      JSON.stringify(watchMsg.TEMPLATE_INDEX) + " (" + typeof watchMsg.TEMPLATE_INDEX + ")"
  );
  assert(
    watchMsg.SLOT_0_CHANNEL === 4 && typeof watchMsg.SLOT_0_CHANNEL === "number",
    "SLOT_0_CHANNEL coerced to a real number, got " + JSON.stringify(watchMsg.SLOT_0_CHANNEL)
  );
  assert(
    watchMsg.DOT_GROUP_SLOT === 2 && watchMsg.DOT_0_CHANNEL === 7 && watchMsg.DOT_SIZE === 0,
    "DOT_GROUP_SLOT/DOT_0_CHANNEL/DOT_SIZE all coerced to real numbers, got " +
      JSON.stringify({ slot: watchMsg.DOT_GROUP_SLOT, ch: watchMsg.DOT_0_CHANNEL, size: watchMsg.DOT_SIZE })
  );
  assert(
    watchMsg.CHARGE_ON_COLOR === "red" && typeof watchMsg.CHARGE_ON_COLOR === "string",
    "CHARGE_ON_COLOR (a color select) is left as a string, not coerced, got " +
      JSON.stringify(watchMsg.CHARGE_ON_COLOR) + " (" + typeof watchMsg.CHARGE_ON_COLOR + ")"
  );
})();

// --- Test 9a-3: every one of the 69 WATCH_SETTINGS_KEYS fields has the JS
// type main.c's config_message.c actually expects off the wire, all sent
// together in one realistic save. This is the type-level counterpart to
// Test 9b's chunking coverage — it exists because Test 9a-2 above only
// sample-checked a few fields, and the real bug (TEMPLATE_INDEX/
// SLOT_n_CHANNEL/DOT_* sent as Clay's native strings instead of numbers)
// was found by manually auditing every field against config_message.c's
// exact reads: 23 fields (TEMPLATE_INDEX, 10x SLOT_n_CHANNEL,
// 6x REPORT_ENABLE_*, DOT_GROUP_SLOT, 4x DOT_n_CHANNEL, DOT_SIZE) are read
// with a blind dict_find(...)->value->int32 and MUST arrive as numbers;
// the other 45 (color/hide-when selects) are read behind a
// `type == TUPLE_CSTRING` check and MUST stay strings. Values below mirror
// exactly what real Clay produces for each component: "select" always
// returns a string (even for numeric-looking options), "toggle" returns a
// real boolean. ---
(function testAllConfigFieldTypesMatchWatchExpectations() {
  var env = freshEnv();
  env.fire("ready");

  var claySettings = {
    TEMPLATE_INDEX: "1", // select -> string from Clay
    DOT_GROUP_SLOT: "2",
    DOT_SIZE: "1",
  };
  for (var i = 0; i < 10; i++) {
    claySettings["SLOT_" + i + "_CHANNEL"] = String(i);
  }
  for (var j = 0; j < 4; j++) {
    claySettings["DOT_" + j + "_CHANNEL"] = String(j + 1);
  }
  [
    "REPORT_ENABLE_BATTERY", "REPORT_ENABLE_STEPS", "REPORT_ENABLE_ACTIVITY", "REPORT_ENABLE_SLEEP",
    "REPORT_ENABLE_HEART_RATE", "REPORT_ENABLE_DEVICE_INFO",
  ].forEach(function (key) {
    claySettings[key] = true; // toggle -> real boolean from Clay
  });
  var colorFields = [
    "CHARGE_ON_COLOR", "CHARGE_OFF_COLOR", "CHARGE_HIDE_WHEN",
    "CONN_ON_COLOR", "CONN_OFF_COLOR", "CONN_HIDE_WHEN",
    "HA_CONN_ON_COLOR", "HA_CONN_OFF_COLOR", "HA_CONN_HIDE_WHEN",
  ];
  ["TIME", "DATE", "BATTERY", "STEPS", "CHARGE", "CONN", "ACTIVE_MINUTES", "DISTANCE",
   "ACTIVE_KCAL", "RESTING_KCAL", "SLEEP_MINUTES", "SLEEP_RESTFUL_MINUTES", "HEART_RATE",
   "HA_CONN"].forEach(function (prefix) {
    colorFields.push(prefix + "_BG_COLOR", prefix + "_VALUE_COLOR", prefix + "_LABEL_COLOR");
  });
  colorFields.forEach(function (key) {
    claySettings[key] = "purple"; // select (color/hide-when) -> string from Clay
  });

  assert(
    Object.keys(claySettings).length === 74,
    "test setup sanity check: exactly 74 WATCH_SETTINGS_KEYS fields, got " + Object.keys(claySettings).length
  );

  var pendingCallbacks = [];
  global.Pebble.sendAppMessage = function (dict, onSuccess) {
    env.sentAppMessages.push(dict);
    pendingCallbacks.push(onSuccess);
  };

  env.fire("webviewclosed", { response: clayResponse(claySettings) });
  while (pendingCallbacks.length > 0) {
    pendingCallbacks.shift()();
  }

  var merged = {};
  env.sentAppMessages.forEach(function (chunk) {
    Object.keys(chunk).forEach(function (k) { merged[k] = chunk[k]; });
  });

  var numericKeys = Object.keys(merged).filter(function (k) {
    return /^(TEMPLATE_INDEX|SLOT_\d+_CHANNEL|REPORT_ENABLE_\w+|DOT_GROUP_SLOT|DOT_\d+_CHANNEL|DOT_SIZE)$/.test(k);
  });
  assert(numericKeys.length === 23, "found all 23 numerically-read fields, got " + numericKeys.length);
  numericKeys.forEach(function (k) {
    assert(typeof merged[k] === "number", k + " sent as a number (watch reads it with a blind ->value->int32), got " + typeof merged[k]);
  });

  var stringKeys = Object.keys(merged).filter(function (k) { return numericKeys.indexOf(k) === -1; });
  assert(stringKeys.length === 51, "found all 51 string-read (TUPLE_CSTRING-checked) fields, got " + stringKeys.length);
  stringKeys.forEach(function (k) {
    assert(typeof merged[k] === "string", k + " left as a string (watch requires TUPLE_CSTRING), got " + typeof merged[k]);
  });
})();

// --- Test 9b: a full-size config save (all 69 WATCH_SETTINGS_KEYS, as a
// real Clay save produces) is split into multiple sequential AppMessages
// instead of one giant dictionary — the real bug this guards against: a
// single 69-key send failed immediately on a real phone and wedged the
// AppMessage session for every subsequent send (see sendWatchSettings()'s
// comment in src/pkjs/index.js). Each chunk must wait for the previous
// chunk's ack before the next one goes out, not fire all at once. ---
(function testLargeConfigSplitIntoSequentialChunks() {
  var env = freshEnv();
  env.fire("ready");

  var sentChunks = [];
  var pendingCallbacks = [];
  // Override the default auto-acking mock for this test only, so chunk N+1
  // can be proven not to send until chunk N's callback is manually fired.
  global.Pebble.sendAppMessage = function (dict, onSuccess) {
    sentChunks.push(dict);
    pendingCallbacks.push(onSuccess);
  };

  var fullSettings = {
    TEMPLATE_INDEX: 1,
    REPORT_ENABLE_BATTERY: true,
    HA_URL: "http://ha.local:8123", // must still be withheld from the watch
    HA_TOKEN: "testtoken",
  };
  for (var i = 0; i < 10; i++) {
    fullSettings["SLOT_" + i + "_CHANNEL"] = i;
  }
  for (var j = 0; j < 4; j++) {
    fullSettings["DOT_" + j + "_CHANNEL"] = j;
  }

  env.fire("webviewclosed", { response: clayResponse(fullSettings) });

  var expectedKeyCount = Object.keys(fullSettings).length - 2; // minus HA_URL/HA_TOKEN
  var expectedChunkCount = Math.ceil(expectedKeyCount / 15);
  assert(expectedChunkCount > 1, "a config this size actually needs more than one chunk (test setup sanity check)");

  // Only the first chunk should have been sent so far — later chunks must
  // wait for earlier ones' acks, not fire all at once synchronously.
  assert(sentChunks.length === 1, "only the first chunk is sent before any ack, got " + sentChunks.length);

  // Resolve acks one at a time; each must release exactly one more chunk.
  var i2 = 0;
  while (i2 < pendingCallbacks.length) {
    var before = sentChunks.length;
    pendingCallbacks[i2]();
    i2++;
    var after = sentChunks.length;
    assert(after - before <= 1, "resolving one chunk's ack sends at most one more chunk, got " + (after - before) + " new");
  }

  assert(
    sentChunks.length === expectedChunkCount,
    "split into ceil(" + expectedKeyCount + "/15) = " + expectedChunkCount + " chunks total, got " + sentChunks.length
  );
  sentChunks.forEach(function (chunk, idx) {
    assert(Object.keys(chunk).length <= 15, "chunk " + idx + " has at most 15 keys, got " + Object.keys(chunk).length);
  });

  var allKeys = {};
  sentChunks.forEach(function (chunk) {
    Object.keys(chunk).forEach(function (k) {
      assert(!(k in allKeys), "key " + k + " sent in exactly one chunk, not duplicated across chunks");
      allKeys[k] = chunk[k];
    });
  });
  assert(!("HA_URL" in allKeys) && !("HA_TOKEN" in allKeys), "HA_URL/HA_TOKEN withheld across all chunks");
  assert(
    Object.keys(allKeys).length === expectedKeyCount,
    "all " + expectedKeyCount + " forwarded keys eventually delivered across chunks, got " + Object.keys(allKeys).length
  );
})();

// --- Test 9c: the HA WebSocket reconnect triggered by a config save must
// not start until config delivery has fully settled (succeeded, or given up
// after exhausting retries) — a config save is the only moment this app
// does an AppMessage burst (chunks) and a WebSocket reconnect (its own
// auth round trips, plus the initial channel snapshot relaying as more
// AppMessages through the same queue) at once; real-device testing showed
// config saves reliably failing while isolated sends worked fine, and
// sequencing these removes the overlap as a suspect. ---
(function testHaReconnectWaitsForConfigDeliveryToSettle() {
  var env = freshEnv();
  env.fire("ready");
  var firstSocket = env.getLastSocket();

  var attempts = 0;
  global.Pebble.sendAppMessage = function (dict, onSuccess, onError) {
    attempts++;
    onError({ error: "Failed to connect to Pebble" });
  };

  env.fire(
    "webviewclosed",
    { response: clayResponse({ TEMPLATE_INDEX: 1, HA_URL: "http://ha.local:8123", HA_TOKEN: "testtoken" }) }
  );

  assert(attempts === 1, "first send attempt happens immediately, got " + attempts);
  assert(
    env.getLastSocket() === firstSocket,
    "HA reconnect has not started while config delivery is still retrying"
  );

  // Drive every retry by always firing the most recently scheduled timer —
  // each failed attempt schedules exactly one new retry, until the final
  // (5th) attempt, which gives up instead.
  while (attempts < 5) {
    env.scheduledTimeouts[env.scheduledTimeouts.length - 1].cb();
  }

  assert(attempts === 5, "config chunk exhausted all 5 attempts, got " + attempts);
  assert(
    env.getLastSocket() !== firstSocket,
    "HA reconnect starts once config delivery has settled, even having given up"
  );
})();

// --- Test 9d: when config delivery succeeds normally (no retries needed),
// the HA reconnect still only starts after it settles, not concurrently ---
(function testHaReconnectWaitsForSuccessfulConfigDelivery() {
  var env = freshEnv();
  env.fire("ready");
  var firstSocket = env.getLastSocket();

  var sendOrder = [];
  global.Pebble.sendAppMessage = function (dict, onSuccess) {
    sendOrder.push("appmessage:" + Object.keys(dict).join(","));
    onSuccess();
  };

  env.fire(
    "webviewclosed",
    { response: clayResponse({ TEMPLATE_INDEX: 1, HA_URL: "http://ha.local:8123", HA_TOKEN: "testtoken" }) }
  );

  assert(
    env.getLastSocket() !== firstSocket,
    "HA reconnect starts immediately after a config chunk that succeeds with no retries needed"
  );
  assert(
    sendOrder.length === 1 && sendOrder[0].indexOf("TEMPLATE_INDEX") !== -1,
    "the config AppMessage was actually sent before the reconnect, got " + JSON.stringify(sendOrder)
  );
})();

// --- Test 10: a watch status report (via appmessage) relays as
// pebble_dashboard/report_status with lowercased, un-prefixed keys ---
(function testStatusReportRelay() {
  var env = freshEnv();
  env.authenticate();
  var socket = env.getLastSocket();
  var sentBeforeReport = socket.sent.length;

  env.sendAppMessageFromWatch({
    REPORT_BATTERY_PERCENT: 87,
    REPORT_BATTERY_CHARGING: 0,
    REPORT_STEPS: 4213,
    REPORT_HEART_RATE_BPM: 72,
  });

  assert(socket.sent.length === sentBeforeReport + 1, "sent exactly one report_status command");
  var reportMsg = socket.sent[socket.sent.length - 1];
  assert(
    reportMsg.type === "pebble_dashboard/report_status" && typeof reportMsg.id === "number",
    "report has the right command type and a request id, got " + JSON.stringify(reportMsg)
  );
  assert(
    reportMsg.status.battery_percent === 87 &&
      reportMsg.status.battery_charging === 0 &&
      reportMsg.status.steps === 4213 &&
      reportMsg.status.heart_rate_bpm === 72,
    "status fields lowercased/un-prefixed correctly, got " + JSON.stringify(reportMsg.status)
  );
})();

// --- Test 10b: REPORT_DISABLED (a string, unlike the numeric fields above)
// relays through the same generic path without special-casing ---
(function testDisabledGroupsRelay() {
  var env = freshEnv();
  env.authenticate();
  var socket = env.getLastSocket();
  var sentBeforeReport = socket.sent.length;

  env.sendAppMessageFromWatch({
    REPORT_BATTERY_PERCENT: 87,
    REPORT_DISABLED: "heart_rate,sleep",
  });

  var reportMsg = socket.sent[socket.sent.length - 1];
  assert(socket.sent.length === sentBeforeReport + 1, "sent exactly one report_status command");
  assert(
    reportMsg.status.disabled === "heart_rate,sleep" && reportMsg.status.battery_percent === 87,
    "string-valued disabled field relayed alongside numeric fields, got " + JSON.stringify(reportMsg.status)
  );
})();

// --- Test 10c: every remaining report_status field (active_seconds,
// distance_meters, active_kcal, resting_kcal, sleep_seconds,
// sleep_restful_seconds, model, firmware, color) relays correctly —
// testStatusReportRelay/testDisabledGroupsRelay above only exercise a
// subset. `connected` is deliberately not part of the watch's own report
// (see checkWatchConnection() in src/pkjs/index.js and its own tests) —
// it's reported by the phone independently, not relayed from here. ---
(function testAllStatusReportFieldsRelay() {
  var env = freshEnv();
  env.authenticate();
  var socket = env.getLastSocket();

  env.sendAppMessageFromWatch({
    REPORT_ACTIVE_SECONDS: 1800,
    REPORT_DISTANCE_METERS: 3120,
    REPORT_ACTIVE_KCAL: 210,
    REPORT_RESTING_KCAL: 2918,
    REPORT_SLEEP_SECONDS: 0,
    REPORT_SLEEP_RESTFUL_SECONDS: 0,
    REPORT_MODEL: "Pebble Time 2",
    REPORT_FIRMWARE: "4.4.1",
    REPORT_COLOR: "Black",
  });

  var reportMsg = socket.sent[socket.sent.length - 1];
  assert(
    reportMsg.status.active_seconds === 1800 &&
      reportMsg.status.distance_meters === 3120 &&
      reportMsg.status.active_kcal === 210 &&
      reportMsg.status.resting_kcal === 2918 &&
      reportMsg.status.sleep_seconds === 0 &&
      reportMsg.status.sleep_restful_seconds === 0 &&
      reportMsg.status.model === "Pebble Time 2" &&
      reportMsg.status.firmware === "4.4.1" &&
      reportMsg.status.color === "Black",
    "all remaining report_status fields relayed correctly, got " + JSON.stringify(reportMsg.status)
  );
})();

// --- Test 11: status reports are dropped (not queued) if not yet authenticated ---
(function testStatusReportDroppedBeforeAuth() {
  var env = freshEnv();
  env.fire("ready"); // socket opened, but no auth_ok yet
  var socket = env.getLastSocket();

  env.sendAppMessageFromWatch({ REPORT_BATTERY_PERCENT: 50 });

  assert(socket.sent.length === 0, "nothing sent to HA before authentication completes, got " + socket.sent.length);
})();

// --- Test 12: a payload with no REPORT_* keys sends nothing ---
(function testEmptyReportSendsNothing() {
  var env = freshEnv();
  env.authenticate();
  var socket = env.getLastSocket();
  var sentBeforeReport = socket.sent.length;

  env.sendAppMessageFromWatch({});

  assert(socket.sent.length === sentBeforeReport, "no report_status command sent for an empty payload");
})();

if (failures > 0) {
  console.error("\n" + failures + " CHECK(S) FAILED");
  process.exitCode = 1;
} else {
  console.log("\nALL CHECKS PASSED");
}
