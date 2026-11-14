#pragma once
#include <pebble.h>

// Shared sizing/protocol constants — referenced across multiple modules,
// so they live here rather than being redefined or passed around.
#define NUM_CONFIGURABLE_SLOTS 10  // must match the SLOT_n_CHANNEL messageKeys in package.json
#define NUM_HA_CHANNELS 10  // must match the HAn_VALUE/HAn_LABEL messageKeys in package.json
#define MAX_DOTS 4
#define NUM_LOCAL_COLOR_CHANNELS 14

// Channel values below must stay in sync with config.js's CHANNEL_OPTIONS:
// 0=TIME, 1=DATE, 2=BATTERY, 3=STEPS, 4=HA1, 5=HA2, 6=HA3,
// 7=BATTERY_CHARGING, 8=CONNECTED, 9=ACTIVE_MINUTES, 10=DISTANCE,
// 11=ACTIVE_KCAL, 12=RESTING_KCAL, 13=SLEEP_MINUTES, 14=SLEEP_RESTFUL_MINUTES,
// 15=HEART_RATE, 16=HA4, 17=HA5, 18=HA6, 19=HA7, 20=HA8, 21=HA9, 22=HA10,
// 23=HA_CONNECTED, 24=DAY_NAME, 25=DATE_SHORT, 26=YEAR. The new ones
// (7-15, then 16-22, then 23, then 24-26) are each appended after whatever
// came before them rather than interleaved, so a previously-persisted slot
// assignment keeps meaning the same channel.
typedef enum {
    CHANNEL_TIME,
    CHANNEL_DATE,
    CHANNEL_BATTERY,
    CHANNEL_STEPS,
    CHANNEL_HA_1,
    CHANNEL_HA_2,
    CHANNEL_HA_3,
    // Appended after the original 7 (rather than interleaved) so a
    // previously-persisted SLOT_n_CHANNEL value keeps meaning the same
    // channel across app updates. Same underlying data as the outbound
    // status report, but local display is independent of the
    // REPORT_ENABLE_* toggles — those only gate what's sent to HA.
    CHANNEL_BATTERY_CHARGING,
    CHANNEL_CONNECTED,
    CHANNEL_ACTIVE_MINUTES,
    CHANNEL_DISTANCE,
    CHANNEL_ACTIVE_KCAL,
    CHANNEL_RESTING_KCAL,
    CHANNEL_SLEEP_MINUTES,
    CHANNEL_SLEEP_RESTFUL_MINUTES,
    CHANNEL_HEART_RATE,
    // HA channels 4-10, appended after the original 16 (not next to
    // HA_1..HA_3) for the same reason the 9 local channels above were
    // appended after the original 7: a previously-persisted SLOT_n_CHANNEL
    // value must keep meaning the same channel across this update.
    CHANNEL_HA_4,
    CHANNEL_HA_5,
    CHANNEL_HA_6,
    CHANNEL_HA_7,
    CHANNEL_HA_8,
    CHANNEL_HA_9,
    CHANNEL_HA_10,
    // The phone's live connection state to Home Assistant (not the HA-side
    // integration's state, and not the watch<->phone Bluetooth link that
    // CHANNEL_CONNECTED already tracks). Unlike every channel above, this
    // one has no on-watch API to read it from at all — pkjs pushes it via
    // HA_CONN_VALUE whenever its own WebSocket auth state changes (see
    // haAuthenticated in src/pkjs/index.js). Appended last, after
    // CHANNEL_HA_10, for the same persisted-slot-compatibility reason as
    // every other group above.
    CHANNEL_HA_CONNECTED,
    // Split-out pieces of the day/date/year row used by the card dashboard
    // template (SLOT_SIZE_CARD, build_template_2() in layout.c) — CHANNEL_DATE
    // already combines all three ("%a %d %b") and its format can't change
    // without altering templates 0/1, so these are new, narrower channels
    // rather than a reformat of an existing one. Display-only: unlike the
    // channels above, these are deliberately not in local_color_channels[]
    // (no Clay-configurable colors) since they're single-purpose, not
    // general-purpose like CHANNEL_HA_1..10/CHANNEL_CONNECTED.
    CHANNEL_DAY_NAME,
    CHANNEL_DATE_SHORT,
    CHANNEL_YEAR,
    NUM_CHANNELS,
} ChannelIndex;

typedef enum {
    CHANNEL_KIND_NUMERIC,
    CHANNEL_KIND_TEXT,
    CHANNEL_KIND_BINARY,
} ChannelKind;

typedef enum {
    CHANNEL_STYLE_RAW,  // render as text (value + optional unit, text, or ON/OFF)
    CHANNEL_STYLE_BAR,  // render as a progress bar (numeric channels with a range only)
} ChannelStyle;

typedef struct {
    ChannelKind kind;
    ChannelStyle style;
    char label[8];
    int32_t value;      // numeric value, or 0/1 for binary
    int32_t min;
    int32_t max;
    bool has_range;
    char unit[8];       // optional unit suffix for numeric channels, e.g. "%"
    char text[16];       // used when kind == CHANNEL_KIND_TEXT
    // Status-dot rendering, used only when this channel is assigned into
    // the dot group (see status_dots.h/.c). on_color/off_color are names
    // from a small shared palette ("red"/"orange"/"yellow"/"green"/"blue"/
    // "purple"/"white"/"gray") — the same vocabulary Clay uses for local
    // channels and HA sends for remote ones. hide_when: 0=always show,
    // 1=hide when this channel is "on", 2=hide when "off".
    char on_color[8];
    char off_color[8];
    int8_t hide_when;
    // General-purpose slot styling, independent of the dot on/off colors
    // above — applies whenever this channel is drawn in any slot, not just
    // as a dot. Empty string = "no override": falls back to whatever color
    // the slot's size class would normally use (see rendering.c), so an
    // unconfigured channel looks exactly like it always has. label_color
    // only has an effect in SLOT_SIZE_SMALL/SLOT_SIZE_CARD, the only size
    // classes that render a label at all. bg_color has no visible effect on
    // SLOT_SIZE_CARD specifically — a card always self-fills white/black,
    // see draw_channel_card() in rendering.c.
    char bg_color[8];
    char value_color[8];
    char label_color[8];
} ChannelData;

// A slot is a screen position from a layout template. Its size class
// controls how the assigned channel is rendered (big centered value vs.
// a label+value stat row) independent of which channel feeds it — any
// channel can be assigned to any slot.
typedef enum {
    SLOT_SIZE_HERO,
    SLOT_SIZE_MEDIUM,
    SLOT_SIZE_SMALL,
    // A bordered card: label on top, big value below (or an enlarged bar
    // for style==BAR channels) — see draw_channel_card() in rendering.c.
    // Font size adapts to the slot's actual height at draw time rather
    // than needing a second "small card" enum value.
    SLOT_SIZE_CARD,
} SlotSizeClass;

typedef struct {
    GRect rect;
    SlotSizeClass size_class;
    ChannelIndex channel;  // default assignment; overridable per-slot by Clay config
} SlotSpec;

typedef struct {
    ChannelIndex channel_index;
    SlotSizeClass size_class;
    // This slot's own index among the configurable slots (0..
    // NUM_CONFIGURABLE_SLOTS-1). Compared against the dot group's target
    // slot at draw time (not baked into a bool at creation) so changing
    // which slot shows dots only needs mark_all_slots_dirty(), not a
    // layout rebuild.
    int slot_index;
} SlotRenderInfo;

// Dot size is user-configurable (Clay), not a fixed constant — defaults to
// the largest, which was the only size before this option existed.
typedef enum {
    DOT_SIZE_SMALL,
    DOT_SIZE_MEDIUM,
    DOT_SIZE_LARGE,
    NUM_DOT_SIZES,
} DotSizeOption;

// Groups of outbound status-report fields (see health_report.h/.c), each
// independently toggleable via Clay config.
typedef enum {
    REPORT_GROUP_BATTERY,
    REPORT_GROUP_STEPS,
    REPORT_GROUP_ACTIVITY,     // active seconds, distance, active + resting kcal
    REPORT_GROUP_SLEEP,        // sleep seconds, restful sleep seconds
    REPORT_GROUP_HEART_RATE,
    // REPORT_GROUP_CONNECTED removed — a "connected" field was close to
    // tautological (if the watch couldn't reach the phone, the report
    // carrying that field wouldn't have arrived either) and carried no
    // real information HA didn't already have from the report's own
    // arrival. Not preserved as a gap: this project has no published
    // installs yet, so there's no persisted PERSIST_KEY_REPORT_ENABLED_BASE
    // state to stay compatible with.
    REPORT_GROUP_DEVICE_INFO,  // model, firmware, color (sent once at startup)
    NUM_REPORT_GROUPS,
} ReportGroup;
