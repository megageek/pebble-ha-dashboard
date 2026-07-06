// Clay configuration schema for the HA Dashboard watchface.
//
// Channel values below must stay in sync with the ChannelIndex enum in
// src/c/main.c: 0=TIME, 1=DATE, 2=BATTERY, 3=STEPS, 4=HA1, 5=HA2, 6=HA3,
// 7=BATTERY_CHARGING, 8=CONNECTED, 9=ACTIVE_MINUTES, 10=DISTANCE,
// 11=ACTIVE_KCAL, 12=RESTING_KCAL, 13=SLEEP_MINUTES, 14=SLEEP_RESTFUL_MINUTES,
// 15=HEART_RATE, 16=HA4, 17=HA5, 18=HA6, 19=HA7, 20=HA8, 21=HA9, 22=HA10.
// The new ones (7-15, then 16-22) are each appended after whatever came
// before them rather than interleaved, so a previously-persisted slot
// assignment keeps meaning the same channel.
var CHANNEL_OPTIONS = [
  { label: "Time", value: 0 },
  { label: "Date", value: 1 },
  { label: "Battery", value: 2 },
  { label: "Steps", value: 3 },
  { label: "HA channel 1", value: 4 },
  { label: "HA channel 2", value: 5 },
  { label: "HA channel 3", value: 6 },
  { label: "Battery charging", value: 7 },
  { label: "Phone connected", value: 8 },
  { label: "Active minutes", value: 9 },
  { label: "Distance walked", value: 10 },
  { label: "Active calories", value: 11 },
  { label: "Resting calories", value: 12 },
  { label: "Sleep minutes", value: 13 },
  { label: "Restful sleep minutes", value: 14 },
  { label: "Heart rate", value: 15 },
  { label: "HA channel 4", value: 16 },
  { label: "HA channel 5", value: 17 },
  { label: "HA channel 6", value: 18 },
  { label: "HA channel 7", value: 19 },
  { label: "HA channel 8", value: 20 },
  { label: "HA channel 9", value: 21 },
  { label: "HA channel 10", value: 22 },
];

function slotSelect(slotIndex, defaultChannel) {
  return {
    type: "select",
    messageKey: "SLOT_" + slotIndex + "_CHANNEL",
    label: "Slot " + (slotIndex + 1) + " channel",
    defaultValue: defaultChannel,
    options: CHANNEL_OPTIONS,
  };
}

// Only channels that can meaningfully be "on"/"off" make sense as a status
// dot — local binary channels, plus the 10 HA channels (which can carry
// whatever on/off meaning HA gives them via value="on"/"off").
var DOT_CHANNEL_OPTIONS = [
  { label: "None", value: -1 },
  { label: "Battery charging", value: 7 },
  { label: "Phone connected", value: 8 },
  { label: "HA channel 1", value: 4 },
  { label: "HA channel 2", value: 5 },
  { label: "HA channel 3", value: 6 },
  { label: "HA channel 4", value: 16 },
  { label: "HA channel 5", value: 17 },
  { label: "HA channel 6", value: 18 },
  { label: "HA channel 7", value: 19 },
  { label: "HA channel 8", value: 20 },
  { label: "HA channel 9", value: 21 },
  { label: "HA channel 10", value: 22 },
];

// Shared with HA's on_color/off_color fields (see HA_INTEGRATION_SPEC.md)
// so there's one color vocabulary everywhere, not a separate Clay-only set.
var COLOR_OPTIONS = [
  { label: "Red", value: "red" },
  { label: "Orange", value: "orange" },
  { label: "Yellow", value: "yellow" },
  { label: "Green", value: "green" },
  { label: "Blue", value: "blue" },
  { label: "Purple", value: "purple" },
  { label: "White", value: "white" },
  { label: "Gray", value: "gray" },
];

var HIDE_WHEN_OPTIONS = [
  { label: "Always show", value: "none" },
  { label: "Hide when ON", value: "on" },
  { label: "Hide when OFF", value: "off" },
];

function dotChannelSelect(dotIndex) {
  return {
    type: "select",
    messageKey: "DOT_" + dotIndex + "_CHANNEL",
    label: "Dot " + (dotIndex + 1),
    defaultValue: -1,
    options: DOT_CHANNEL_OPTIONS,
  };
}

function colorSelect(messageKey, label, defaultValue) {
  return {
    type: "select",
    messageKey: messageKey,
    label: label,
    defaultValue: defaultValue,
    options: COLOR_OPTIONS,
  };
}

function hideWhenSelect(messageKey, label) {
  return {
    type: "select",
    messageKey: messageKey,
    label: label,
    defaultValue: "none",
    options: HIDE_WHEN_OPTIONS,
  };
}

module.exports = [
  {
    type: "heading",
    defaultValue: "HA Dashboard Settings",
    size: 1,
  },
  {
    type: "select",
    messageKey: "TEMPLATE_INDEX",
    label: "Layout template",
    defaultValue: 0,
    options: [
      { label: "Clock hero + 3 stats", value: 0 },
      { label: "Battery hero + 4 stats", value: 1 },
    ],
  },
  {
    type: "heading",
    defaultValue: "Slot assignments",
    size: 3,
  },
  {
    type: "text",
    defaultValue:
      "Both current templates use only the first 5 slots. Slots 6-10 " +
      "are here for future/custom layouts and are ignored by either " +
      "template today.",
  },
  // Defaults mirror Template 0's slot assignments (see build_template_0
  // in main.c): hero=Time, medium=Date, small rows=Battery/Steps/HA1.
  slotSelect(0, 0),
  slotSelect(1, 1),
  slotSelect(2, 2),
  slotSelect(3, 3),
  slotSelect(4, 4),
  // Slots 5-9: not used by either built-in template, default is arbitrary.
  slotSelect(5, 0),
  slotSelect(6, 0),
  slotSelect(7, 0),
  slotSelect(8, 0),
  slotSelect(9, 0),
  {
    type: "heading",
    defaultValue: "Status Dots",
    size: 3,
  },
  {
    type: "text",
    defaultValue:
      "Show up to 4 small colored dots inside one slot, in addition to " +
      "that slot's normal value — good for binary things that don't need " +
      "a whole slot of their own.",
  },
  {
    type: "select",
    messageKey: "DOT_GROUP_SLOT",
    label: "Show dots in slot",
    defaultValue: -1,
    options: [
      { label: "None", value: -1 },
      { label: "Slot 1", value: 0 },
      { label: "Slot 2", value: 1 },
      { label: "Slot 3", value: 2 },
      { label: "Slot 4", value: 3 },
      { label: "Slot 5", value: 4 },
      { label: "Slot 6", value: 5 },
      { label: "Slot 7", value: 6 },
      { label: "Slot 8", value: 7 },
      { label: "Slot 9", value: 8 },
      { label: "Slot 10", value: 9 },
    ],
  },
  dotChannelSelect(0),
  dotChannelSelect(1),
  dotChannelSelect(2),
  dotChannelSelect(3),
  {
    type: "select",
    messageKey: "DOT_SIZE",
    label: "Dot size",
    defaultValue: 2,
    options: [
      { label: "Small", value: 0 },
      { label: "Medium", value: 1 },
      { label: "Large", value: 2 },
    ],
  },
  {
    type: "text",
    defaultValue:
      "Colors for the two local dot-capable channels below. HA channels " +
      "set their own colors when pushing a value (see HA_INTEGRATION_SPEC.md).",
  },
  colorSelect("CHARGE_ON_COLOR", "Battery charging: ON color", "green"),
  colorSelect("CHARGE_OFF_COLOR", "Battery charging: OFF color", "gray"),
  hideWhenSelect("CHARGE_HIDE_WHEN", "Battery charging: visibility"),
  colorSelect("CONN_ON_COLOR", "Phone connected: ON color", "green"),
  colorSelect("CONN_OFF_COLOR", "Phone connected: OFF color", "red"),
  hideWhenSelect("CONN_HIDE_WHEN", "Phone connected: visibility"),
  {
    type: "heading",
    defaultValue: "Status Reporting",
    size: 3,
  },
  {
    type: "text",
    defaultValue:
      "Choose what the watch reports back to Home Assistant. Turning " +
      "something off tells HA so it can remove any entity it created for it.",
  },
  {
    type: "toggle",
    messageKey: "REPORT_ENABLE_BATTERY",
    label: "Battery",
    defaultValue: true,
  },
  {
    type: "toggle",
    messageKey: "REPORT_ENABLE_STEPS",
    label: "Steps",
    defaultValue: true,
  },
  {
    type: "toggle",
    messageKey: "REPORT_ENABLE_ACTIVITY",
    label: "Activity (active time, distance, calories)",
    defaultValue: true,
  },
  {
    type: "toggle",
    messageKey: "REPORT_ENABLE_SLEEP",
    label: "Sleep",
    defaultValue: true,
  },
  {
    type: "toggle",
    messageKey: "REPORT_ENABLE_HEART_RATE",
    label: "Heart rate",
    defaultValue: true,
  },
  {
    type: "toggle",
    messageKey: "REPORT_ENABLE_CONNECTED",
    label: "Connection status",
    defaultValue: true,
  },
  {
    type: "toggle",
    messageKey: "REPORT_ENABLE_DEVICE_INFO",
    label: "Device info (model/firmware/color)",
    defaultValue: true,
  },
  {
    type: "heading",
    defaultValue: "Home Assistant Connection",
    size: 3,
  },
  {
    type: "text",
    defaultValue:
      "These stay on your phone — they're used by pkjs to open a " +
      "WebSocket to Home Assistant and are never sent to the watch.",
  },
  {
    type: "input",
    messageKey: "HA_URL",
    label: "Home Assistant URL",
    defaultValue: "",
    attributes: {
      placeholder: "http://homeassistant.local:8123",
    },
  },
  {
    type: "input",
    messageKey: "HA_TOKEN",
    label: "Long-lived access token",
    defaultValue: "",
    attributes: {
      type: "password",
      placeholder: "Paste your token here",
    },
  },
  {
    type: "submit",
    defaultValue: "Save",
  },
];
