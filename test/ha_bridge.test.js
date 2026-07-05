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

var failures = 0;

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
  var scheduledTimeouts = [];
  var lastSocket = null;

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
      sentAppMessages.push(dict);
      if (onSuccess) onSuccess();
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
  MockWebSocket.prototype.close = function () {};
  global.WebSocket = MockWebSocket;

  global.setTimeout = function (cb, delay) {
    var id = scheduledTimeouts.length;
    scheduledTimeouts.push({ cb: cb, delay: delay });
    return id;
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

  function sendChannelEvent(channel, value, label) {
    var data = { channel: channel, value: value };
    if (label !== undefined) {
      data.label = label;
    }
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
    sentAppMessages: sentAppMessages,
    scheduledTimeouts: scheduledTimeouts,
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
  assert(socket.sent.length === 2, "sends exactly one more message after auth_ok, got " + socket.sent.length);
  assert(
    socket.sent[1].type === HA_SUBSCRIBE_COMMAND && typeof socket.sent[1].id === "number",
    "sends the subscribe_channels command with a request id, got " + JSON.stringify(socket.sent[1])
  );
})();

// --- Test 2: initial state arrives via the subscribe command's "result" reply ---
(function testInitialStateViaResult() {
  var env = freshEnv();
  env.authenticate();

  env.sendInitialResult([
    { channel: 1, value: "72F", label: "Temp" },
    { channel: 2, value: "Closed" }, // no label — channel keeps its default
    { channel: 9, value: "ignored" }, // out of range, must not relay
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

  env.sendChannelEvent(1, "72F", "Temp");
  env.sendChannelEvent(2, "Open", "Door");
  env.sendChannelEvent(3, "On", "Light");

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

  env.sendChannelEvent(2, "73F"); // no label passed

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
  env.sendChannelEvent(1, longValue, longLabel);

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

// --- Test 7: unknown channel numbers are ignored, not relayed ---
(function testUnknownChannelIgnored() {
  var env = freshEnv();
  env.authenticate();

  env.sendChannelEvent(0, "x", "y");
  env.sendChannelEvent(4, "x", "y");
  env.sendChannelEvent("not-a-number", "x", "y");

  assert(env.sentAppMessages.length === 0, "no AppMessage sent for out-of-range channels, got " + env.sentAppMessages.length);
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

// --- Test 9: webviewclosed filters HA_URL/HA_TOKEN out of the AppMessage
// sent to the watch, but still reconnects using the new values ---
(function testWebviewClosedFiltersAndReconnects() {
  var env = freshEnv();
  env.fire("ready");
  var firstSocket = env.getLastSocket();
  assert(firstSocket.url === "http://ha.local:8123/api/websocket", "initial connect uses the original URL");

  // A flat JSON string (starts with "{") is accepted by Clay.getSettings()
  // without needing URI-decoding — see its `response.match(/^\{/)` check.
  var claySavePayload = JSON.stringify({
    TEMPLATE_INDEX: 1,
    SLOT_0_CHANNEL: 4,
    SLOT_1_CHANNEL: 1,
    SLOT_2_CHANNEL: 2,
    SLOT_3_CHANNEL: 3,
    SLOT_4_CHANNEL: 0,
    HA_URL: "http://new-ha.local:8123",
    HA_TOKEN: "newtoken",
  });

  env.fire("webviewclosed", { response: claySavePayload });

  assert(env.sentAppMessages.length === 1, "config save sent exactly one AppMessage to the watch");
  var watchMsg = env.sentAppMessages[0];
  assert(watchMsg.TEMPLATE_INDEX === 1, "TEMPLATE_INDEX forwarded to the watch, got " + JSON.stringify(watchMsg));
  assert(watchMsg.SLOT_0_CHANNEL === 4, "SLOT_0_CHANNEL forwarded to the watch");
  assert(!("HA_URL" in watchMsg) && !("HA_TOKEN" in watchMsg), "HA_URL/HA_TOKEN withheld from the watch, got " + JSON.stringify(watchMsg));

  var secondSocket = env.getLastSocket();
  assert(
    secondSocket !== firstSocket && secondSocket.url === "http://new-ha.local:8123/api/websocket",
    "webviewclosed reconnected using the newly saved URL, got " + secondSocket.url
  );
})();

if (failures > 0) {
  console.error("\n" + failures + " CHECK(S) FAILED");
  process.exitCode = 1;
} else {
  console.log("\nALL CHECKS PASSED");
}
