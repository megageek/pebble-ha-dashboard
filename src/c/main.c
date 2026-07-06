#include <pebble.h>

#define MAX_SLOTS 10
#define NUM_CONFIGURABLE_SLOTS 10  // must match the SLOT_n_CHANNEL messageKeys in package.json
#define NUM_HA_CHANNELS 10  // must match the HAn_VALUE/HAn_LABEL messageKeys in package.json

#define PERSIST_KEY_TEMPLATE 100
#define PERSIST_KEY_SLOT_BASE 101  // slot i -> PERSIST_KEY_SLOT_BASE + i
#define PERSIST_KEY_REPORT_ENABLED_BASE 200  // group i -> PERSIST_KEY_REPORT_ENABLED_BASE + i

#define MAX_DOTS 4
#define DOT_AREA_MARGIN 8  // gap between the dot cluster and the slot's normal content
#define PERSIST_KEY_DOT_GROUP_SLOT 299  // which slot (0..NUM_CONFIGURABLE_SLOTS-1) also shows dots, -1 = none
#define PERSIST_KEY_DOT_CHANNEL_BASE 300  // dot i -> PERSIST_KEY_DOT_CHANNEL_BASE + i
#define PERSIST_KEY_DOT_SIZE 326
#define PERSIST_KEY_CHARGE_ON_COLOR 320
#define PERSIST_KEY_CHARGE_OFF_COLOR 321
#define PERSIST_KEY_CHARGE_HIDE_WHEN 322
#define PERSIST_KEY_CONN_ON_COLOR 323
#define PERSIST_KEY_CONN_OFF_COLOR 324
#define PERSIST_KEY_CONN_HIDE_WHEN 325

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
    // the dot group (see "Status dot group" below). on_color/off_color are
    // names from a small shared palette ("red"/"orange"/"yellow"/"green"/
    // "blue"/"purple"/"white"/"gray") — the same vocabulary Clay uses for
    // local channels and HA sends for remote ones. hide_when: 0=always
    // show, 1=hide when this channel is "on", 2=hide when "off".
    char on_color[8];
    char off_color[8];
    int8_t hide_when;
} ChannelData;

// A slot is a screen position from a layout template. Its size class
// controls how the assigned channel is rendered (big centered value vs.
// a label+value stat row) independent of which channel feeds it — any
// channel can be assigned to any slot.
typedef enum {
    SLOT_SIZE_HERO,
    SLOT_SIZE_MEDIUM,
    SLOT_SIZE_SMALL,
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
    // NUM_CONFIGURABLE_SLOTS-1). Compared against s_dot_group_slot at draw
    // time (not baked into a bool at creation) so changing which slot
    // shows dots only needs mark_all_slots_dirty(), not a layout rebuild.
    int slot_index;
} SlotRenderInfo;

static Window *s_main_window;
static Layer *s_separator_layer;
static Layer *s_slot_layers[MAX_SLOTS];
static int s_slot_count = 0;

static ChannelData s_channels[NUM_CHANNELS];

static int s_active_template = 0;
// -1 means "use the template's default channel for this slot".
static int s_slot_channel_override[NUM_CONFIGURABLE_SLOTS];

// Status dot group: up to MAX_DOTS channels rendered as small colored
// circles, for binary-ish measures that don't warrant a full slot of their
// own. One shared group per layout (not per-slot), attached to whichever
// configurable slot index s_dot_group_slot names — additively, alongside
// that slot's normal channel content, not replacing it. -1 = disabled /
// no channel assigned to that dot position.
static int s_dot_group_slot = -1;  // which of the 10 configurable slots also shows dots; -1 = none
static int s_dot_channels[MAX_DOTS];

// Dot size is user-configurable (Clay), not a fixed constant — defaults to
// the largest, which was the only size before this option existed.
typedef enum {
    DOT_SIZE_SMALL,
    DOT_SIZE_MEDIUM,
    DOT_SIZE_LARGE,
    NUM_DOT_SIZES,
} DotSizeOption;

static const int DOT_DIAMETERS[NUM_DOT_SIZES] = { 6, 9, 12 };
static const int DOT_SPACINGS[NUM_DOT_SIZES] = { 5, 6, 8 };
static int s_dot_size = DOT_SIZE_LARGE;

// Groups of outbound status-report fields (see "Outbound reporting" below),
// each independently toggleable via Clay config.
typedef enum {
    REPORT_GROUP_BATTERY,
    REPORT_GROUP_STEPS,
    REPORT_GROUP_ACTIVITY,     // active seconds, distance, active + resting kcal
    REPORT_GROUP_SLEEP,        // sleep seconds, restful sleep seconds
    REPORT_GROUP_HEART_RATE,
    REPORT_GROUP_CONNECTED,
    REPORT_GROUP_DEVICE_INFO,  // model, firmware, color (sent once at startup)
    NUM_REPORT_GROUPS,
} ReportGroup;

static const char *const REPORT_GROUP_NAMES[NUM_REPORT_GROUPS] = {
    "battery", "steps", "activity", "sleep", "heart_rate", "connected", "device_info",
};

// All enabled by default; persisted overrides loaded in load_config().
static bool s_report_enabled[NUM_REPORT_GROUPS] = {
    true, true, true, true, true, true, true,
};

static void init_channels(void) {
    s_channels[CHANNEL_TIME] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "TIME",
    };
    s_channels[CHANNEL_DATE] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "DATE",
    };
    s_channels[CHANNEL_BATTERY] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_BAR, .label = "BATT",
        .min = 0, .max = 100, .has_range = true, .unit = "%",
    };
    s_channels[CHANNEL_STEPS] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "STEPS",
    };
    // Remote channels pushed by Home Assistant via the pkjs WebSocket bridge.
    // Text until first update arrives; HA can repurpose a channel's kind
    // implicitly (values always arrive as strings over AppMessage today).
    // Default dot colors before HA ever sends its own — HA can override
    // on_color/off_color/hide_when per channel at any time (see
    // inbox_received_callback()).
    s_channels[CHANNEL_HA_1] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA1", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_2] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA2", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_3] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA3", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_4] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA4", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_5] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA5", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_6] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA6", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_7] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA7", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_8] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA8", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_9] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA9", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_10] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA10", .text = "--",
        .on_color = "green", .off_color = "red",
    };

    // Local health/status channels — same source data as the outbound
    // status report, but selectable into any slot regardless of whether
    // reporting to HA is enabled for that measure. Populated by
    // update_health_channels()/update_battery()/battery_callback(); kind
    // may flip to TEXT ("N/A") there if a metric isn't accessible. Dot
    // colors here are defaults; overridable via Clay (see load_config()).
    s_channels[CHANNEL_BATTERY_CHARGING] = (ChannelData) {
        .kind = CHANNEL_KIND_BINARY, .style = CHANNEL_STYLE_RAW, .label = "CHARGE",
        .on_color = "green", .off_color = "gray",
    };
    s_channels[CHANNEL_CONNECTED] = (ChannelData) {
        .kind = CHANNEL_KIND_BINARY, .style = CHANNEL_STYLE_RAW, .label = "CONN",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_ACTIVE_MINUTES] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "ACTIVE", .unit = "min",
    };
    s_channels[CHANNEL_DISTANCE] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "DIST", .unit = "m",
    };
    s_channels[CHANNEL_ACTIVE_KCAL] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "ACAL", .unit = "kcal",
    };
    s_channels[CHANNEL_RESTING_KCAL] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "RCAL", .unit = "kcal",
    };
    s_channels[CHANNEL_SLEEP_MINUTES] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "SLEEP", .unit = "min",
    };
    s_channels[CHANNEL_SLEEP_RESTFUL_MINUTES] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "RSLEEP", .unit = "min",
    };
    s_channels[CHANNEL_HEART_RATE] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "HR", .unit = "bpm",
    };
}

// Template 0: reproduces the original fixed layout (hero clock, date,
// 3 stat rows) expressed as channel-agnostic slots, so any channel can
// later be reassigned to any of these positions.
static int build_template_0(GRect bounds, SlotSpec *out) {
    int n = 0;

    const int hero_top = 24;
    const int hero_height = 60;
    out[n++] = (SlotSpec) {
        GRect(0, hero_top, bounds.size.w, hero_height), SLOT_SIZE_HERO, CHANNEL_TIME,
    };

    const int date_top = hero_top + hero_height + 4;
    const int date_height = 28;
    out[n++] = (SlotSpec) {
        GRect(0, date_top, bounds.size.w, date_height), SLOT_SIZE_MEDIUM, CHANNEL_DATE,
    };

    const int separator_top = date_top + date_height + 6;
    const int stats_top = separator_top + 10;
    const int bottom_margin = 6;
    const int num_stats = 3;
    const int available = bounds.size.h - stats_top - bottom_margin;
    const int row_height = available / num_stats;
    const ChannelIndex stat_channels[3] = { CHANNEL_BATTERY, CHANNEL_STEPS, CHANNEL_HA_1 };

    for (int i = 0; i < num_stats; i++) {
        int row_top = stats_top + i * row_height;
        out[n++] = (SlotSpec) {
            GRect(0, row_top, bounds.size.w, row_height), SLOT_SIZE_SMALL, stat_channels[i],
        };
    }

    return n;
}

// Template 1: a battery-forward layout — a numeric+range channel shown
// hero-sized (with its unit, since HERO always renders as text) above
// four small stat rows instead of three. Proves slots vary in both
// size and count from Template 0, and that any channel can fill any
// slot (battery moves from a small bar row to the hero position; time
// and date drop from hero/medium down to small stat rows).
static int build_template_1(GRect bounds, SlotSpec *out) {
    int n = 0;

    const int hero_top = 20;
    const int hero_height = 56;
    out[n++] = (SlotSpec) {
        GRect(0, hero_top, bounds.size.w, hero_height), SLOT_SIZE_HERO, CHANNEL_BATTERY,
    };

    const int stats_top = hero_top + hero_height + 16;  // separator gap + breathing room
    const int bottom_margin = 6;
    const int num_stats = 4;
    const int available = bounds.size.h - stats_top - bottom_margin;
    const int row_height = available / num_stats;
    const ChannelIndex stat_channels[4] = {
        CHANNEL_TIME, CHANNEL_DATE, CHANNEL_STEPS, CHANNEL_HA_1,
    };

    for (int i = 0; i < num_stats; i++) {
        int row_top = stats_top + i * row_height;
        out[n++] = (SlotSpec) {
            GRect(0, row_top, bounds.size.w, row_height), SLOT_SIZE_SMALL, stat_channels[i],
        };
    }

    return n;
}

typedef int (*TemplateBuilder)(GRect bounds, SlotSpec *out);
static const TemplateBuilder s_templates[] = { build_template_0, build_template_1 };
#define NUM_TEMPLATES ((int)(sizeof(s_templates) / sizeof(s_templates[0])))

// Loads template choice and per-slot channel overrides persisted by a
// previous Clay config save, falling back to defaults (template 0, no
// overrides) on first run or if a stored value is out of range.
static void load_config(void) {
    s_active_template = 0;
    if (persist_exists(PERSIST_KEY_TEMPLATE)) {
        int stored = persist_read_int(PERSIST_KEY_TEMPLATE);
        if (stored >= 0 && stored < NUM_TEMPLATES) {
            s_active_template = stored;
        }
    }

    for (int i = 0; i < NUM_CONFIGURABLE_SLOTS; i++) {
        s_slot_channel_override[i] = -1;
        if (persist_exists(PERSIST_KEY_SLOT_BASE + i)) {
            int stored = persist_read_int(PERSIST_KEY_SLOT_BASE + i);
            if (stored >= 0 && stored < NUM_CHANNELS) {
                s_slot_channel_override[i] = stored;
            }
        }
    }

    for (int i = 0; i < NUM_REPORT_GROUPS; i++) {
        s_report_enabled[i] = true;
        if (persist_exists(PERSIST_KEY_REPORT_ENABLED_BASE + i)) {
            s_report_enabled[i] = persist_read_int(PERSIST_KEY_REPORT_ENABLED_BASE + i) != 0;
        }
    }

    s_dot_group_slot = -1;
    if (persist_exists(PERSIST_KEY_DOT_GROUP_SLOT)) {
        int stored = persist_read_int(PERSIST_KEY_DOT_GROUP_SLOT);
        if (stored == -1 || (stored >= 0 && stored < NUM_CONFIGURABLE_SLOTS)) {
            s_dot_group_slot = stored;
        }
    }

    for (int i = 0; i < MAX_DOTS; i++) {
        s_dot_channels[i] = -1;
        if (persist_exists(PERSIST_KEY_DOT_CHANNEL_BASE + i)) {
            int stored = persist_read_int(PERSIST_KEY_DOT_CHANNEL_BASE + i);
            if (stored == -1 || (stored >= 0 && stored < NUM_CHANNELS)) {
                s_dot_channels[i] = stored;
            }
        }
    }

    s_dot_size = DOT_SIZE_LARGE;
    if (persist_exists(PERSIST_KEY_DOT_SIZE)) {
        int stored = persist_read_int(PERSIST_KEY_DOT_SIZE);
        if (stored >= 0 && stored < NUM_DOT_SIZES) {
            s_dot_size = stored;
        }
    }

    // Local channel dot colors/hide-when; defaults already set in
    // init_channels(), only overridden here if a Clay save persisted one.
    if (persist_exists(PERSIST_KEY_CHARGE_ON_COLOR)) {
        persist_read_string(PERSIST_KEY_CHARGE_ON_COLOR, s_channels[CHANNEL_BATTERY_CHARGING].on_color,
                             sizeof(s_channels[CHANNEL_BATTERY_CHARGING].on_color));
    }
    if (persist_exists(PERSIST_KEY_CHARGE_OFF_COLOR)) {
        persist_read_string(PERSIST_KEY_CHARGE_OFF_COLOR, s_channels[CHANNEL_BATTERY_CHARGING].off_color,
                             sizeof(s_channels[CHANNEL_BATTERY_CHARGING].off_color));
    }
    if (persist_exists(PERSIST_KEY_CHARGE_HIDE_WHEN)) {
        s_channels[CHANNEL_BATTERY_CHARGING].hide_when = (int8_t)persist_read_int(PERSIST_KEY_CHARGE_HIDE_WHEN);
    }
    if (persist_exists(PERSIST_KEY_CONN_ON_COLOR)) {
        persist_read_string(PERSIST_KEY_CONN_ON_COLOR, s_channels[CHANNEL_CONNECTED].on_color,
                             sizeof(s_channels[CHANNEL_CONNECTED].on_color));
    }
    if (persist_exists(PERSIST_KEY_CONN_OFF_COLOR)) {
        persist_read_string(PERSIST_KEY_CONN_OFF_COLOR, s_channels[CHANNEL_CONNECTED].off_color,
                             sizeof(s_channels[CHANNEL_CONNECTED].off_color));
    }
    if (persist_exists(PERSIST_KEY_CONN_HIDE_WHEN)) {
        s_channels[CHANNEL_CONNECTED].hide_when = (int8_t)persist_read_int(PERSIST_KEY_CONN_HIDE_WHEN);
    }
}

static void mark_all_slots_dirty(void) {
    for (int i = 0; i < s_slot_count; i++) {
        layer_mark_dirty(s_slot_layers[i]);
    }
}

// Separator sits just below the lowest non-stat-row slot, whichever
// template is active and however many such slots it has.
static int compute_separator_top(SlotSpec *specs, int count) {
    int header_bottom = 0;
    for (int i = 0; i < count; i++) {
        if (specs[i].size_class != SLOT_SIZE_SMALL) {
            int bottom = specs[i].rect.origin.y + specs[i].rect.size.h;
            if (bottom > header_bottom) {
                header_bottom = bottom;
            }
        }
    }
    return header_bottom + 6;
}

static void separator_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(0, 0), GPoint(bounds.size.w, 0));
}

static void draw_channel_bar(GContext *ctx, GRect bounds, ChannelData *ch) {
    const int bar_height = 12;
    GRect bar_frame = GRect(bounds.origin.x, bounds.origin.y + (bounds.size.h - bar_height) / 2,
                             bounds.size.w, bar_height);

    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_draw_rect(ctx, bar_frame);

    int32_t range = ch->max - ch->min;
    int32_t clamped = ch->value;
    if (clamped < ch->min) clamped = ch->min;
    if (clamped > ch->max) clamped = ch->max;

    int fill_width = 0;
    if (range > 0) {
        fill_width = (int)(((clamped - ch->min) * (bar_frame.size.w - 2)) / range);
    }

    if (fill_width > 0) {
        GRect fill_frame = GRect(bar_frame.origin.x + 1, bar_frame.origin.y + 1,
                                  fill_width, bar_frame.size.h - 2);
        graphics_context_set_fill_color(ctx, GColorWhite);
        graphics_fill_rect(ctx, fill_frame, 0, GCornerNone);
    }
}

static void draw_channel_text_aligned(GContext *ctx, GRect bounds, ChannelData *ch,
                                       GFont font, GTextAlignment alignment, GColor color) {
    char buffer[24];
    const char *text = buffer;

    switch (ch->kind) {
        case CHANNEL_KIND_TEXT:
            text = ch->text;
            break;
        case CHANNEL_KIND_BINARY:
            text = ch->value ? "ON" : "OFF";
            break;
        case CHANNEL_KIND_NUMERIC:
        default:
            if (ch->unit[0] != '\0') {
                snprintf(buffer, sizeof(buffer), "%ld%s", (long)ch->value, ch->unit);
            } else {
                snprintf(buffer, sizeof(buffer), "%ld", (long)ch->value);
            }
            break;
    }

    graphics_context_set_text_color(ctx, color);
    graphics_draw_text(ctx, text, font, bounds, GTextOverflowModeTrailingEllipsis, alignment, NULL);
}

static void draw_channel_small_row(GContext *ctx, GRect bounds, ChannelData *ch) {
    int half = bounds.size.w / 2;
    GRect label_rect = GRect(bounds.origin.x + 14, bounds.origin.y, half - 14, bounds.size.h);
    GRect value_rect = GRect(bounds.origin.x + half, bounds.origin.y, half - 14, bounds.size.h);

    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, ch->label, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), label_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    if (ch->style == CHANNEL_STYLE_BAR && ch->kind == CHANNEL_KIND_NUMERIC && ch->has_range) {
        draw_channel_bar(ctx, value_rect, ch);
    } else {
        draw_channel_text_aligned(ctx, value_rect, ch, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                                   GTextAlignmentRight, GColorWhite);
    }
}

// ---------------------------------------------------------------------------
// Status dot group — a row of small colored circles for binary-ish
// measures that don't warrant a full label+value slot. See
// CHANNEL_DOT_GROUP_MARKER / s_dot_channels above.
// ---------------------------------------------------------------------------

// Named palette shared with Clay (local channel color pickers) and with
// HA (on_color/off_color fields in pebble_channel_update) — see
// HA_INTEGRATION_SPEC.md. Unrecognized/empty names fall back to light gray
// rather than failing, since a color typo shouldn't crash rendering.
static GColor color_from_name(const char *name) {
    if (strcmp(name, "red") == 0) return GColorRed;
    if (strcmp(name, "orange") == 0) return GColorOrange;
    if (strcmp(name, "yellow") == 0) return GColorYellow;
    if (strcmp(name, "green") == 0) return GColorGreen;
    if (strcmp(name, "blue") == 0) return GColorBlue;
    if (strcmp(name, "purple") == 0) return GColorPurple;
    if (strcmp(name, "white") == 0) return GColorWhite;
    if (strcmp(name, "gray") == 0) return GColorLightGray;
    return GColorLightGray;
}

// "none"/"on"/"off" -> 0/1/2. Same three-value vocabulary for Clay's local
// hide-when select and HA's hide_when field.
static int8_t parse_hide_when(const char *s) {
    if (strcmp(s, "on") == 0) return 1;
    if (strcmp(s, "off") == 0) return 2;
    return 0;
}

// A channel's "on" state for dot purposes, regardless of its kind: binary
// and numeric channels are on when non-zero; text channels (the only kind
// HA channels can be) are on when the value is exactly the lowercase
// string "on" — see HA_INTEGRATION_SPEC.md for why this is a fixed literal
// rather than a fuzzy/case-insensitive check.
static bool channel_is_on(ChannelData *ch) {
    switch (ch->kind) {
        case CHANNEL_KIND_BINARY:
        case CHANNEL_KIND_NUMERIC:
            return ch->value != 0;
        case CHANNEL_KIND_TEXT:
        default:
            return strcmp(ch->text, "on") == 0;
    }
}

static int count_assigned_dots(void) {
    int count = 0;
    for (int i = 0; i < MAX_DOTS; i++) {
        if (s_dot_channels[i] >= 0) {
            count++;
        }
    }
    return count;
}

// Draws centered within whatever (sub-)rect it's given — the caller
// (slot_update_proc) carves out a reserved strip on the right of the slot
// for this, rather than this function claiming the whole slot.
static void draw_dot_group(GContext *ctx, GRect bounds) {
    int assigned = count_assigned_dots();
    if (assigned == 0) {
        return;
    }

    int diameter = DOT_DIAMETERS[s_dot_size];
    int spacing = DOT_SPACINGS[s_dot_size];
    int total_width = assigned * diameter + (assigned - 1) * spacing;
    int start_x = bounds.origin.x + (bounds.size.w - total_width) / 2;
    int center_y = bounds.origin.y + bounds.size.h / 2;

    // Position is assigned by configured slot, not by "currently visible"
    // slot, so a hidden dot leaves a gap rather than shifting its
    // neighbors — otherwise dots would jump around every time one toggles.
    int slot_i = 0;
    for (int i = 0; i < MAX_DOTS; i++) {
        if (s_dot_channels[i] < 0) {
            continue;
        }
        ChannelData *ch = &s_channels[s_dot_channels[i]];
        bool on = channel_is_on(ch);
        bool hidden = (ch->hide_when == 1 && on) || (ch->hide_when == 2 && !on);

        if (!hidden) {
            int cx = start_x + slot_i * (diameter + spacing) + diameter / 2;
            GColor color = color_from_name(on ? ch->on_color : ch->off_color);
            graphics_context_set_fill_color(ctx, color);
            graphics_fill_circle(ctx, GPoint(cx, center_y), diameter / 2);
        }
        slot_i++;
    }
}

static void slot_update_proc(Layer *layer, GContext *ctx) {
    SlotRenderInfo *info = layer_get_data(layer);
    GRect bounds = layer_get_bounds(layer);
    ChannelData *ch = &s_channels[info->channel_index];

    // Dots reserve a strip on the right, narrowing (not replacing) the
    // area available to the slot's normal channel content — the dots are
    // additive, shown alongside whatever channel is already assigned here.
    GRect content_bounds = bounds;
    if (info->slot_index == s_dot_group_slot) {
        int assigned = count_assigned_dots();
        if (assigned > 0) {
            int diameter = DOT_DIAMETERS[s_dot_size];
            int spacing = DOT_SPACINGS[s_dot_size];
            int dot_area_width = assigned * diameter + (assigned - 1) * spacing + DOT_AREA_MARGIN;
            content_bounds.size.w -= dot_area_width;
            GRect dot_bounds = GRect(bounds.origin.x + bounds.size.w - dot_area_width, bounds.origin.y,
                                      dot_area_width, bounds.size.h);
            draw_dot_group(ctx, dot_bounds);
        }
    }

    switch (info->size_class) {
        case SLOT_SIZE_HERO:
            draw_channel_text_aligned(ctx, content_bounds, ch, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD),
                                       GTextAlignmentCenter, GColorWhite);
            break;
        case SLOT_SIZE_MEDIUM:
            draw_channel_text_aligned(ctx, content_bounds, ch, fonts_get_system_font(FONT_KEY_GOTHIC_24),
                                       GTextAlignmentCenter, GColorLightGray);
            break;
        case SLOT_SIZE_SMALL:
        default:
            draw_channel_small_row(ctx, content_bounds, ch);
            break;
    }
}

// ---------------------------------------------------------------------------
// Outbound reporting: watch -> phone -> Home Assistant
//
// The watch reports its own status back out via AppMessage; pkjs relays it
// to HA over the same WebSocket connection used to receive channel data
// (see HA_INTEGRATION_SPEC.md). Every metric here is read from a service the
// watchface already touches for its own display (battery, steps) or is a
// cheap additional read at the same MINUTE_UNIT cadence — nothing here adds
// a new wake-up source or extra battery cost. Heart rate deliberately reads
// whatever the system's own default background sampling already produced
// (health_service_peek_current_value()) rather than requesting an elevated
// sample rate, so it's exactly as cheap as everything else here.
//
// Each measure belongs to a ReportGroup the user can disable via Clay config
// (see the top of the file); disabling one doesn't just stop sending its
// fields — every report also carries a REPORT_DISABLED list of currently-
// disabled group names, so the HA-side integration can tell "never available
// on this hardware" (field simply absent) apart from "user turned this off"
// (group named in REPORT_DISABLED) and remove/deactivate the right entities
// accordingly.
// ---------------------------------------------------------------------------

static void write_health_metric_if_available(DictionaryIterator *iter, uint32_t key,
                                              HealthMetric metric, time_t start, time_t end) {
    HealthServiceAccessibilityMask mask = health_service_metric_accessible(metric, start, end);
    if (mask & HealthServiceAccessibilityMaskAvailable) {
        HealthValue value = health_service_sum(metric, start, end);
        dict_write_int32(iter, key, (int32_t)value);
    }
}

// Comma-joins the names of every currently-disabled group so HA can tell
// "user turned this off" apart from "not accessible on this hardware" (the
// latter is just a field that's silently absent, same as it always was).
static void write_disabled_groups(DictionaryIterator *iter) {
    char buffer[80];
    buffer[0] = '\0';
    bool any = false;

    for (int i = 0; i < NUM_REPORT_GROUPS; i++) {
        if (s_report_enabled[i]) {
            continue;
        }
        if (any) {
            strncat(buffer, ",", sizeof(buffer) - strlen(buffer) - 1);
        }
        strncat(buffer, REPORT_GROUP_NAMES[i], sizeof(buffer) - strlen(buffer) - 1);
        any = true;
    }

    if (any) {
        dict_write_cstring(iter, MESSAGE_KEY_REPORT_DISABLED, buffer);
    }
}

static void send_status_report(void) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
        return;  // outbox busy; the next periodic call will try again
    }

    if (s_report_enabled[REPORT_GROUP_BATTERY]) {
        BatteryChargeState battery = battery_state_service_peek();
        dict_write_int32(iter, MESSAGE_KEY_REPORT_BATTERY_PERCENT, battery.charge_percent);
        dict_write_int32(iter, MESSAGE_KEY_REPORT_BATTERY_CHARGING, battery.is_charging ? 1 : 0);
    }

    if (s_report_enabled[REPORT_GROUP_CONNECTED]) {
        dict_write_int32(iter, MESSAGE_KEY_REPORT_CONNECTED,
                          connection_service_peek_pebble_app_connection() ? 1 : 0);
    }

    time_t day_start = time_start_of_today();
    time_t now = time(NULL);

    if (s_report_enabled[REPORT_GROUP_STEPS]) {
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_STEPS, HealthMetricStepCount, day_start, now);
    }

    if (s_report_enabled[REPORT_GROUP_ACTIVITY]) {
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_ACTIVE_SECONDS, HealthMetricActiveSeconds, day_start, now);
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_DISTANCE_METERS, HealthMetricWalkedDistanceMeters, day_start, now);
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_ACTIVE_KCAL, HealthMetricActiveKCalories, day_start, now);
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_RESTING_KCAL, HealthMetricRestingKCalories, day_start, now);
    }

    if (s_report_enabled[REPORT_GROUP_SLEEP]) {
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_SLEEP_SECONDS, HealthMetricSleepSeconds, day_start, now);
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_SLEEP_RESTFUL_SECONDS, HealthMetricSleepRestfulSeconds, day_start, now);
    }

    // Heart rate is an instantaneous reading, not a daily sum — accessibility
    // is checked for "right now", and health_service_peek_current_value()
    // (not _sum_today()) returns the latest sample. This deliberately relies
    // on the system's own default background HR sampling rather than
    // requesting an elevated sample rate — whatever value is already cached
    // (per Pebble's docs, at most ~15 min old) is what gets reported, no
    // extra battery cost incurred by this watchface.
    if (s_report_enabled[REPORT_GROUP_HEART_RATE]) {
        HealthServiceAccessibilityMask hr_mask = health_service_metric_accessible(HealthMetricHeartRateBPM, now, now);
        if (hr_mask & HealthServiceAccessibilityMaskAvailable) {
            HealthValue hr = health_service_peek_current_value(HealthMetricHeartRateBPM);
            dict_write_int32(iter, MESSAGE_KEY_REPORT_HEART_RATE_BPM, (int32_t)hr);
        }
    }

    write_disabled_groups(iter);

    app_message_outbox_send();
}

static const char *watch_model_name(void) {
    switch (watch_info_get_model()) {
        case WATCH_INFO_MODEL_PEBBLE_TIME_2: return "Pebble Time 2";
        case WATCH_INFO_MODEL_COREDEVICES_PT2: return "Pebble Time 2 (Core Devices)";
        default: return "Unknown";
    }
}

static const char *watch_color_name(void) {
    switch (watch_info_get_color()) {
        case WATCH_INFO_COLOR_PEBBLE_TIME_2_BLACK: return "Black";
        case WATCH_INFO_COLOR_PEBBLE_TIME_2_SILVER: return "Silver";
        case WATCH_INFO_COLOR_PEBBLE_TIME_2_GOLD: return "Gold";
        case WATCH_INFO_COLOR_COREDEVICES_PT2_BLACK_GREY: return "Black/Grey";
        case WATCH_INFO_COLOR_COREDEVICES_PT2_BLACK_RED: return "Black/Red";
        case WATCH_INFO_COLOR_COREDEVICES_PT2_SILVER_BLUE: return "Silver/Blue";
        case WATCH_INFO_COLOR_COREDEVICES_PT2_SILVER_GREY: return "Silver/Grey";
        default: return "Unknown";
    }
}

// Static device info, sent once at startup rather than on every tick.
static void send_device_info(void) {
    if (!s_report_enabled[REPORT_GROUP_DEVICE_INFO]) {
        return;  // its disabled state still surfaces via write_disabled_groups()
    }

    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
        return;
    }

    WatchInfoVersion fw = watch_info_get_firmware_version();
    char fw_buffer[16];
    snprintf(fw_buffer, sizeof(fw_buffer), "%d.%d.%d", fw.major, fw.minor, fw.patch);

    dict_write_cstring(iter, MESSAGE_KEY_REPORT_MODEL, watch_model_name());
    dict_write_cstring(iter, MESSAGE_KEY_REPORT_FIRMWARE, fw_buffer);
    dict_write_cstring(iter, MESSAGE_KEY_REPORT_COLOR, watch_color_name());

    app_message_outbox_send();
}

static void update_time(void) {
    time_t temp = time(NULL);
    struct tm *tick_time = localtime(&temp);
    if (!tick_time) {
        return;
    }

    strftime(s_channels[CHANNEL_TIME].text, sizeof(s_channels[CHANNEL_TIME].text),
              clock_is_24h_style() ? "%H:%M" : "%I:%M", tick_time);
    strftime(s_channels[CHANNEL_DATE].text, sizeof(s_channels[CHANNEL_DATE].text),
              "%a %d %b", tick_time);

    mark_all_slots_dirty();
}

static void update_battery(void) {
    BatteryChargeState state = battery_state_service_peek();
    s_channels[CHANNEL_BATTERY].value = state.charge_percent;
    s_channels[CHANNEL_BATTERY_CHARGING].value = state.is_charging ? 1 : 0;
    mark_all_slots_dirty();
}

// Shared by CHANNEL_STEPS and the health-metric channels in
// update_health_channels(): sets kind=TEXT "N/A" when the metric isn't
// accessible (no permission, unsupported hardware, or no data for the
// range), otherwise kind=NUMERIC with the (optionally scaled, e.g.
// seconds->minutes) summed value.
static void update_numeric_health_channel(ChannelIndex channel, HealthMetric metric,
                                           time_t start, time_t end, int32_t divisor) {
    ChannelData *ch = &s_channels[channel];
    HealthServiceAccessibilityMask mask = health_service_metric_accessible(metric, start, end);
    if (mask & HealthServiceAccessibilityMaskAvailable) {
        ch->kind = CHANNEL_KIND_NUMERIC;
        HealthValue value = health_service_sum(metric, start, end);
        ch->value = divisor > 1 ? (int32_t)value / divisor : (int32_t)value;
    } else {
        ch->kind = CHANNEL_KIND_TEXT;
        strncpy(ch->text, "N/A", sizeof(ch->text) - 1);
        ch->text[sizeof(ch->text) - 1] = '\0';
    }
}

static void update_steps(void) {
    update_numeric_health_channel(CHANNEL_STEPS, HealthMetricStepCount, time_start_of_today(), time(NULL), 1);
    mark_all_slots_dirty();
}

// The rest of the health/status channels — same source data as the
// outbound status report, but always updated regardless of the
// REPORT_ENABLE_* toggles (those only gate what's sent to HA, not what
// the watch itself can display).
static void update_health_channels(void) {
    time_t day_start = time_start_of_today();
    time_t now = time(NULL);

    update_numeric_health_channel(CHANNEL_ACTIVE_MINUTES, HealthMetricActiveSeconds, day_start, now, 60);
    update_numeric_health_channel(CHANNEL_DISTANCE, HealthMetricWalkedDistanceMeters, day_start, now, 1);
    update_numeric_health_channel(CHANNEL_ACTIVE_KCAL, HealthMetricActiveKCalories, day_start, now, 1);
    update_numeric_health_channel(CHANNEL_RESTING_KCAL, HealthMetricRestingKCalories, day_start, now, 1);
    update_numeric_health_channel(CHANNEL_SLEEP_MINUTES, HealthMetricSleepSeconds, day_start, now, 60);
    update_numeric_health_channel(CHANNEL_SLEEP_RESTFUL_MINUTES, HealthMetricSleepRestfulSeconds, day_start, now, 60);

    // Heart rate is instantaneous, not summed — see send_status_report()'s
    // comment for why this deliberately reads the default background
    // sample rather than requesting a fresher one.
    ChannelData *hr = &s_channels[CHANNEL_HEART_RATE];
    HealthServiceAccessibilityMask hr_mask = health_service_metric_accessible(HealthMetricHeartRateBPM, now, now);
    if (hr_mask & HealthServiceAccessibilityMaskAvailable) {
        hr->kind = CHANNEL_KIND_NUMERIC;
        hr->value = (int32_t)health_service_peek_current_value(HealthMetricHeartRateBPM);
    } else {
        hr->kind = CHANNEL_KIND_TEXT;
        strncpy(hr->text, "N/A", sizeof(hr->text) - 1);
        hr->text[sizeof(hr->text) - 1] = '\0';
    }

    s_channels[CHANNEL_CONNECTED].value = connection_service_peek_pebble_app_connection() ? 1 : 0;

    mark_all_slots_dirty();
}

static void battery_callback(BatteryChargeState state) {
    s_channels[CHANNEL_BATTERY].value = state.charge_percent;
    s_channels[CHANNEL_BATTERY_CHARGING].value = state.is_charging ? 1 : 0;
    mark_all_slots_dirty();
    send_status_report();
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    update_time();
    update_steps();
    update_health_channels();
    send_status_report();
}

// Builds slot layers for the active template + config overrides. Safe
// to call again after destroy_active_layout() when config changes.
static void load_active_layout(Layer *window_layer, GRect bounds) {
    SlotSpec specs[MAX_SLOTS];
    s_slot_count = s_templates[s_active_template](bounds, specs);

    for (int i = 0; i < s_slot_count; i++) {
        ChannelIndex channel = specs[i].channel;
        if (i < NUM_CONFIGURABLE_SLOTS && s_slot_channel_override[i] >= 0) {
            channel = (ChannelIndex)s_slot_channel_override[i];
        }

        Layer *slot_layer = layer_create_with_data(specs[i].rect, sizeof(SlotRenderInfo));
        SlotRenderInfo *info = layer_get_data(slot_layer);
        info->channel_index = channel;
        info->size_class = specs[i].size_class;
        info->slot_index = i;
        layer_set_update_proc(slot_layer, slot_update_proc);
        layer_add_child(window_layer, slot_layer);
        s_slot_layers[i] = slot_layer;
    }

    // Divider below the header slot(s) and above the stat rows.
    int separator_top = compute_separator_top(specs, s_slot_count);
    s_separator_layer = layer_create(GRect(10, separator_top, bounds.size.w - 20, 1));
    layer_set_update_proc(s_separator_layer, separator_update_proc);
    layer_add_child(window_layer, s_separator_layer);
}

static void destroy_active_layout(void) {
    for (int i = 0; i < s_slot_count; i++) {
        layer_destroy(s_slot_layers[i]);
        s_slot_layers[i] = NULL;
    }
    s_slot_count = 0;

    if (s_separator_layer) {
        layer_destroy(s_separator_layer);
        s_separator_layer = NULL;
    }
}

// Shared by the two local dot-capable channels (battery charging, phone
// connection) to apply+persist on_color/off_color/hide_when tuples from a
// Clay save. Returns true if anything actually changed, so the caller can
// fold that into its own "does this need a redraw" tracking.
static bool apply_dot_color_config(DictionaryIterator *iterator, ChannelIndex channel,
                                    uint32_t on_color_key, uint32_t off_color_key, uint32_t hide_when_key,
                                    int persist_on_color_key, int persist_off_color_key,
                                    int persist_hide_when_key) {
    bool changed = false;
    ChannelData *ch = &s_channels[channel];

    Tuple *on_tuple = dict_find(iterator, on_color_key);
    if (on_tuple && on_tuple->type == TUPLE_CSTRING) {
        strncpy(ch->on_color, on_tuple->value->cstring, sizeof(ch->on_color) - 1);
        ch->on_color[sizeof(ch->on_color) - 1] = '\0';
        persist_write_string(persist_on_color_key, ch->on_color);
        changed = true;
    }

    Tuple *off_tuple = dict_find(iterator, off_color_key);
    if (off_tuple && off_tuple->type == TUPLE_CSTRING) {
        strncpy(ch->off_color, off_tuple->value->cstring, sizeof(ch->off_color) - 1);
        ch->off_color[sizeof(ch->off_color) - 1] = '\0';
        persist_write_string(persist_off_color_key, ch->off_color);
        changed = true;
    }

    Tuple *hide_tuple = dict_find(iterator, hide_when_key);
    if (hide_tuple && hide_tuple->type == TUPLE_CSTRING) {
        ch->hide_when = parse_hide_when(hide_tuple->value->cstring);
        persist_write_int(persist_hide_when_key, ch->hide_when);
        changed = true;
    }

    return changed;
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
    bool layout_changed = false;

    Tuple *template_tuple = dict_find(iterator, MESSAGE_KEY_TEMPLATE_INDEX);
    if (template_tuple) {
        int value = (int)template_tuple->value->int32;
        if (value >= 0 && value < NUM_TEMPLATES) {
            s_active_template = value;
            persist_write_int(PERSIST_KEY_TEMPLATE, value);
            layout_changed = true;
        }
    }

    const uint32_t slot_keys[NUM_CONFIGURABLE_SLOTS] = {
        MESSAGE_KEY_SLOT_0_CHANNEL, MESSAGE_KEY_SLOT_1_CHANNEL, MESSAGE_KEY_SLOT_2_CHANNEL,
        MESSAGE_KEY_SLOT_3_CHANNEL, MESSAGE_KEY_SLOT_4_CHANNEL, MESSAGE_KEY_SLOT_5_CHANNEL,
        MESSAGE_KEY_SLOT_6_CHANNEL, MESSAGE_KEY_SLOT_7_CHANNEL, MESSAGE_KEY_SLOT_8_CHANNEL,
        MESSAGE_KEY_SLOT_9_CHANNEL,
    };
    for (int i = 0; i < NUM_CONFIGURABLE_SLOTS; i++) {
        Tuple *tuple = dict_find(iterator, slot_keys[i]);
        if (!tuple) {
            continue;
        }
        int value = (int)tuple->value->int32;
        if (value >= 0 && value < NUM_CHANNELS) {
            s_slot_channel_override[i] = value;
            persist_write_int(PERSIST_KEY_SLOT_BASE + i, value);
            layout_changed = true;
        }
    }

    // Remote channel updates pushed by pkjs from Home Assistant. Only
    // redraws the affected slots — never a layout change on their own.
    bool value_changed = false;
    const uint32_t ha_value_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_VALUE, MESSAGE_KEY_HA2_VALUE, MESSAGE_KEY_HA3_VALUE, MESSAGE_KEY_HA4_VALUE,
        MESSAGE_KEY_HA5_VALUE, MESSAGE_KEY_HA6_VALUE, MESSAGE_KEY_HA7_VALUE, MESSAGE_KEY_HA8_VALUE,
        MESSAGE_KEY_HA9_VALUE, MESSAGE_KEY_HA10_VALUE,
    };
    const uint32_t ha_label_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_LABEL, MESSAGE_KEY_HA2_LABEL, MESSAGE_KEY_HA3_LABEL, MESSAGE_KEY_HA4_LABEL,
        MESSAGE_KEY_HA5_LABEL, MESSAGE_KEY_HA6_LABEL, MESSAGE_KEY_HA7_LABEL, MESSAGE_KEY_HA8_LABEL,
        MESSAGE_KEY_HA9_LABEL, MESSAGE_KEY_HA10_LABEL,
    };
    // Optional dot-styling fields HA can send alongside value/label — see
    // HA_INTEGRATION_SPEC.md. Presence isn't what marks a channel as
    // dot-able (any channel can be put in the dot group); these just
    // control how it looks if it is.
    const uint32_t ha_on_color_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_ON_COLOR, MESSAGE_KEY_HA2_ON_COLOR, MESSAGE_KEY_HA3_ON_COLOR, MESSAGE_KEY_HA4_ON_COLOR,
        MESSAGE_KEY_HA5_ON_COLOR, MESSAGE_KEY_HA6_ON_COLOR, MESSAGE_KEY_HA7_ON_COLOR, MESSAGE_KEY_HA8_ON_COLOR,
        MESSAGE_KEY_HA9_ON_COLOR, MESSAGE_KEY_HA10_ON_COLOR,
    };
    const uint32_t ha_off_color_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_OFF_COLOR, MESSAGE_KEY_HA2_OFF_COLOR, MESSAGE_KEY_HA3_OFF_COLOR, MESSAGE_KEY_HA4_OFF_COLOR,
        MESSAGE_KEY_HA5_OFF_COLOR, MESSAGE_KEY_HA6_OFF_COLOR, MESSAGE_KEY_HA7_OFF_COLOR, MESSAGE_KEY_HA8_OFF_COLOR,
        MESSAGE_KEY_HA9_OFF_COLOR, MESSAGE_KEY_HA10_OFF_COLOR,
    };
    const uint32_t ha_hide_when_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_HIDE_WHEN, MESSAGE_KEY_HA2_HIDE_WHEN, MESSAGE_KEY_HA3_HIDE_WHEN, MESSAGE_KEY_HA4_HIDE_WHEN,
        MESSAGE_KEY_HA5_HIDE_WHEN, MESSAGE_KEY_HA6_HIDE_WHEN, MESSAGE_KEY_HA7_HIDE_WHEN, MESSAGE_KEY_HA8_HIDE_WHEN,
        MESSAGE_KEY_HA9_HIDE_WHEN, MESSAGE_KEY_HA10_HIDE_WHEN,
    };
    const ChannelIndex ha_channels[NUM_HA_CHANNELS] = {
        CHANNEL_HA_1, CHANNEL_HA_2, CHANNEL_HA_3, CHANNEL_HA_4, CHANNEL_HA_5,
        CHANNEL_HA_6, CHANNEL_HA_7, CHANNEL_HA_8, CHANNEL_HA_9, CHANNEL_HA_10,
    };

    for (int i = 0; i < NUM_HA_CHANNELS; i++) {
        ChannelData *ch = &s_channels[ha_channels[i]];

        Tuple *value_tuple = dict_find(iterator, ha_value_keys[i]);
        if (value_tuple && value_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->text, value_tuple->value->cstring, sizeof(ch->text) - 1);
            ch->text[sizeof(ch->text) - 1] = '\0';
            ch->kind = CHANNEL_KIND_TEXT;
            value_changed = true;
        }

        Tuple *label_tuple = dict_find(iterator, ha_label_keys[i]);
        if (label_tuple && label_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->label, label_tuple->value->cstring, sizeof(ch->label) - 1);
            ch->label[sizeof(ch->label) - 1] = '\0';
            value_changed = true;
        }

        Tuple *on_color_tuple = dict_find(iterator, ha_on_color_keys[i]);
        if (on_color_tuple && on_color_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->on_color, on_color_tuple->value->cstring, sizeof(ch->on_color) - 1);
            ch->on_color[sizeof(ch->on_color) - 1] = '\0';
            value_changed = true;
        }

        Tuple *off_color_tuple = dict_find(iterator, ha_off_color_keys[i]);
        if (off_color_tuple && off_color_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->off_color, off_color_tuple->value->cstring, sizeof(ch->off_color) - 1);
            ch->off_color[sizeof(ch->off_color) - 1] = '\0';
            value_changed = true;
        }

        Tuple *hide_when_tuple = dict_find(iterator, ha_hide_when_keys[i]);
        if (hide_when_tuple && hide_when_tuple->type == TUPLE_CSTRING) {
            ch->hide_when = parse_hide_when(hide_when_tuple->value->cstring);
            value_changed = true;
        }
    }

    // Per-measure reporting toggles from Clay config. Changing one doesn't
    // affect what's shown on the watch face at all — only what gets reported
    // out — so it's tracked separately from layout_changed/value_changed.
    bool reporting_config_changed = false;
    const uint32_t report_enable_keys[NUM_REPORT_GROUPS] = {
        MESSAGE_KEY_REPORT_ENABLE_BATTERY, MESSAGE_KEY_REPORT_ENABLE_STEPS,
        MESSAGE_KEY_REPORT_ENABLE_ACTIVITY, MESSAGE_KEY_REPORT_ENABLE_SLEEP,
        MESSAGE_KEY_REPORT_ENABLE_HEART_RATE, MESSAGE_KEY_REPORT_ENABLE_CONNECTED,
        MESSAGE_KEY_REPORT_ENABLE_DEVICE_INFO,
    };
    for (int i = 0; i < NUM_REPORT_GROUPS; i++) {
        Tuple *tuple = dict_find(iterator, report_enable_keys[i]);
        if (!tuple) {
            continue;
        }
        bool enabled = tuple->value->int32 != 0;
        if (enabled != s_report_enabled[i]) {
            s_report_enabled[i] = enabled;
            persist_write_int(PERSIST_KEY_REPORT_ENABLED_BASE + i, enabled ? 1 : 0);
            reporting_config_changed = true;
        }
    }

    // Status dot group: which slot shows it, which channels feed it, and
    // (for the two local dot-capable channels) their colors/hide-when.
    // None of this changes slot rects/count, only what a slot draws, so
    // it's redraw-only (mark_all_slots_dirty()) — never a layout rebuild.
    bool dot_config_changed = false;

    Tuple *dot_group_slot_tuple = dict_find(iterator, MESSAGE_KEY_DOT_GROUP_SLOT);
    if (dot_group_slot_tuple) {
        int value = (int)dot_group_slot_tuple->value->int32;
        if (value == -1 || (value >= 0 && value < NUM_CONFIGURABLE_SLOTS)) {
            s_dot_group_slot = value;
            persist_write_int(PERSIST_KEY_DOT_GROUP_SLOT, value);
            dot_config_changed = true;
        }
    }

    const uint32_t dot_channel_keys[MAX_DOTS] = {
        MESSAGE_KEY_DOT_0_CHANNEL, MESSAGE_KEY_DOT_1_CHANNEL,
        MESSAGE_KEY_DOT_2_CHANNEL, MESSAGE_KEY_DOT_3_CHANNEL,
    };
    for (int i = 0; i < MAX_DOTS; i++) {
        Tuple *tuple = dict_find(iterator, dot_channel_keys[i]);
        if (!tuple) {
            continue;
        }
        int value = (int)tuple->value->int32;
        if (value == -1 || (value >= 0 && value < NUM_CHANNELS)) {
            s_dot_channels[i] = value;
            persist_write_int(PERSIST_KEY_DOT_CHANNEL_BASE + i, value);
            dot_config_changed = true;
        }
    }

    Tuple *dot_size_tuple = dict_find(iterator, MESSAGE_KEY_DOT_SIZE);
    if (dot_size_tuple) {
        int value = (int)dot_size_tuple->value->int32;
        if (value >= 0 && value < NUM_DOT_SIZES) {
            s_dot_size = value;
            persist_write_int(PERSIST_KEY_DOT_SIZE, value);
            dot_config_changed = true;
        }
    }

    if (apply_dot_color_config(iterator, CHANNEL_BATTERY_CHARGING, MESSAGE_KEY_CHARGE_ON_COLOR,
                                MESSAGE_KEY_CHARGE_OFF_COLOR, MESSAGE_KEY_CHARGE_HIDE_WHEN,
                                PERSIST_KEY_CHARGE_ON_COLOR, PERSIST_KEY_CHARGE_OFF_COLOR,
                                PERSIST_KEY_CHARGE_HIDE_WHEN)) {
        dot_config_changed = true;
    }
    if (apply_dot_color_config(iterator, CHANNEL_CONNECTED, MESSAGE_KEY_CONN_ON_COLOR,
                                MESSAGE_KEY_CONN_OFF_COLOR, MESSAGE_KEY_CONN_HIDE_WHEN,
                                PERSIST_KEY_CONN_ON_COLOR, PERSIST_KEY_CONN_OFF_COLOR,
                                PERSIST_KEY_CONN_HIDE_WHEN)) {
        dot_config_changed = true;
    }

    if (!layout_changed && !value_changed && !reporting_config_changed && !dot_config_changed) {
        return;
    }

    if (layout_changed) {
        Layer *window_layer = window_get_root_layer(s_main_window);
        GRect bounds = layer_get_bounds(window_layer);
        destroy_active_layout();
        load_active_layout(window_layer, bounds);

        update_time();
        update_battery();
        update_steps();
        update_health_channels();
    } else if (value_changed || dot_config_changed) {
        mark_all_slots_dirty();
    }

    // Report promptly on a toggle change instead of waiting up to a minute
    // for the next tick — this is exactly the "let HA know now" signal the
    // disabled-groups list exists for.
    if (reporting_config_changed) {
        send_status_report();
    }
}

static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    window_set_background_color(window, GColorBlack);

    init_channels();
    load_config();
    load_active_layout(window_layer, bounds);

    update_time();
    update_battery();
    update_steps();
    update_health_channels();
}

static void main_window_unload(Window *window) {
    destroy_active_layout();
}

static void init(void) {
    s_main_window = window_create();
    window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load = main_window_load,
        .unload = main_window_unload,
    });
    window_stack_push(s_main_window, true);

    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
    battery_state_service_subscribe(battery_callback);

    app_message_register_inbox_received(inbox_received_callback);
    app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());

    send_device_info();
    send_status_report();
}

static void deinit(void) {
    tick_timer_service_unsubscribe();
    battery_state_service_unsubscribe();
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
    return 0;
}
