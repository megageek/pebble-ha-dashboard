// Clay configuration schema for the HA Dashboard watchface.
//
// Channel values below must stay in sync with the ChannelIndex enum in
// src/c/main.c: 0=TIME, 1=DATE, 2=BATTERY, 3=STEPS, 4=HA1, 5=HA2, 6=HA3.
var CHANNEL_OPTIONS = [
  { label: "Time", value: 0 },
  { label: "Date", value: 1 },
  { label: "Battery", value: 2 },
  { label: "Steps", value: 3 },
  { label: "HA channel 1", value: 4 },
  { label: "HA channel 2", value: 5 },
  { label: "HA channel 3", value: 6 },
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
      "Both templates have 5 slots. Slots beyond what the chosen " +
      "template uses are ignored.",
  },
  // Defaults mirror Template 0's slot assignments (see build_template_0
  // in main.c): hero=Time, medium=Date, small rows=Battery/Steps/HA1.
  slotSelect(0, 0),
  slotSelect(1, 1),
  slotSelect(2, 2),
  slotSelect(3, 3),
  slotSelect(4, 4),
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
