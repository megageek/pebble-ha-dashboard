var Clay = require("pebble-clay");
var clayConfig = require("./config");
var clay = new Clay(clayConfig);

Pebble.addEventListener("showConfiguration", function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener("webviewclosed", function (e) {
  if (!e || !e.response) {
    return;
  }

  var settings = clay.getSettings(e.response);
  Pebble.sendAppMessage(
    settings,
    function () {
      console.log("Config sent to watch");
    },
    function (err) {
      console.log("Failed to send config: " + JSON.stringify(err));
    }
  );
});
