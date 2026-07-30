#include "config_message.h"
#include "channels.h"
#include "layout.h"
#include "status_dots.h"
#include "health_report.h"
#include "channel_updates.h"
#include <string.h>

#define PERSIST_KEY_TEMPLATE 100
#define PERSIST_KEY_SLOT_BASE 101  // slot i -> PERSIST_KEY_SLOT_BASE + i
#define PERSIST_KEY_REPORT_ENABLED_BASE 200  // group i -> PERSIST_KEY_REPORT_ENABLED_BASE + i

#define PERSIST_KEY_DOT_GROUP_SLOT 299  // which slot (0..NUM_CONFIGURABLE_SLOTS-1) also shows dots, -1 = none
#define PERSIST_KEY_DOT_CHANNEL_BASE 300  // dot i -> PERSIST_KEY_DOT_CHANNEL_BASE + i
#define PERSIST_KEY_DOT_SIZE 326
#define PERSIST_KEY_CHARGE_ON_COLOR 320
#define PERSIST_KEY_CHARGE_OFF_COLOR 321
#define PERSIST_KEY_CHARGE_HIDE_WHEN 322
#define PERSIST_KEY_CONN_ON_COLOR 323
#define PERSIST_KEY_CONN_OFF_COLOR 324
#define PERSIST_KEY_CONN_HIDE_WHEN 325

// General-purpose slot styling (background/value/label colors), Clay-
// configurable per local channel; HA sets these directly on its own remote
// channels over the wire instead (see apply_ha_channel_updates() below).
// Channel i (index into local_color_channels[]) field f (0=bg,1=value,
// 2=label) -> PERSIST_KEY_LOCAL_COLOR_BASE + i*3 + f.
#define PERSIST_KEY_LOCAL_COLOR_BASE 400  // through 400 + 13*3 - 1 = 438

void load_config(void) {
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

    // General-purpose background/value/label colors for the 13 local
    // channels. Empty (unset) is the default already left in place by
    // init_channels(), so only overridden here if a Clay save persisted one.
    for (int i = 0; i < NUM_LOCAL_COLOR_CHANNELS; i++) {
        ChannelData *ch = &s_channels[local_color_channels[i]];
        int bg_key = PERSIST_KEY_LOCAL_COLOR_BASE + i * 3 + 0;
        int value_key = PERSIST_KEY_LOCAL_COLOR_BASE + i * 3 + 1;
        int label_key = PERSIST_KEY_LOCAL_COLOR_BASE + i * 3 + 2;

        if (persist_exists(bg_key)) {
            persist_read_string(bg_key, ch->bg_color, sizeof(ch->bg_color));
        }
        if (persist_exists(value_key)) {
            persist_read_string(value_key, ch->value_color, sizeof(ch->value_color));
        }
        if (persist_exists(label_key)) {
            persist_read_string(label_key, ch->label_color, sizeof(ch->label_color));
        }
    }
}

// Applies HAn_VALUE/LABEL/KIND/UNIT/MIN/MAX/STYLE/ON_COLOR/OFF_COLOR/
// HIDE_WHEN/BG_COLOR/VALUE_COLOR/LABEL_COLOR for all 10 HA channels, i.e.
// everything HA can push for its own remote channels. Pulled out of
// inbox_received_callback() into its own function purely to keep that
// function's top-level flow readable as a short sequence of named steps —
// this one block alone was pushing 200+ lines and 14 key arrays inline.
static bool apply_ha_channel_updates(DictionaryIterator *iterator) {
    bool changed = false;

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
    // General-purpose slot styling HA can send alongside value/label — same
    // idea as on_color/off_color above but for a channel's normal (non-dot)
    // appearance. Not persisted on the watch; HA is the source of truth and
    // resends these on every reconnect via the initial subscribe result.
    const uint32_t ha_bg_color_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_BG_COLOR, MESSAGE_KEY_HA2_BG_COLOR, MESSAGE_KEY_HA3_BG_COLOR, MESSAGE_KEY_HA4_BG_COLOR,
        MESSAGE_KEY_HA5_BG_COLOR, MESSAGE_KEY_HA6_BG_COLOR, MESSAGE_KEY_HA7_BG_COLOR, MESSAGE_KEY_HA8_BG_COLOR,
        MESSAGE_KEY_HA9_BG_COLOR, MESSAGE_KEY_HA10_BG_COLOR,
    };
    const uint32_t ha_value_color_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_VALUE_COLOR, MESSAGE_KEY_HA2_VALUE_COLOR, MESSAGE_KEY_HA3_VALUE_COLOR, MESSAGE_KEY_HA4_VALUE_COLOR,
        MESSAGE_KEY_HA5_VALUE_COLOR, MESSAGE_KEY_HA6_VALUE_COLOR, MESSAGE_KEY_HA7_VALUE_COLOR, MESSAGE_KEY_HA8_VALUE_COLOR,
        MESSAGE_KEY_HA9_VALUE_COLOR, MESSAGE_KEY_HA10_VALUE_COLOR,
    };
    const uint32_t ha_label_color_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_LABEL_COLOR, MESSAGE_KEY_HA2_LABEL_COLOR, MESSAGE_KEY_HA3_LABEL_COLOR, MESSAGE_KEY_HA4_LABEL_COLOR,
        MESSAGE_KEY_HA5_LABEL_COLOR, MESSAGE_KEY_HA6_LABEL_COLOR, MESSAGE_KEY_HA7_LABEL_COLOR, MESSAGE_KEY_HA8_LABEL_COLOR,
        MESSAGE_KEY_HA9_LABEL_COLOR, MESSAGE_KEY_HA10_LABEL_COLOR,
    };
    // Optional numeric/binary support (see HA_INTEGRATION_SPEC.md). Kind is
    // sticky (only updated when sent) and defaults to CHANNEL_KIND_TEXT from
    // init_channels() until an integration ever sends one — existing
    // text-only integrations keep working unmodified. min/max only take
    // effect when both are present in the *same* message (see below), so
    // there's no separate "min set but max isn't yet" persisted state to
    // track. style defaults to CHANNEL_STYLE_RAW and only matters once
    // kind=numeric and a range exists (rendering.c's draw_channel_small_row()
    // already requires all three for bar rendering, same as local channels).
    const uint32_t ha_kind_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_KIND, MESSAGE_KEY_HA2_KIND, MESSAGE_KEY_HA3_KIND, MESSAGE_KEY_HA4_KIND,
        MESSAGE_KEY_HA5_KIND, MESSAGE_KEY_HA6_KIND, MESSAGE_KEY_HA7_KIND, MESSAGE_KEY_HA8_KIND,
        MESSAGE_KEY_HA9_KIND, MESSAGE_KEY_HA10_KIND,
    };
    const uint32_t ha_unit_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_UNIT, MESSAGE_KEY_HA2_UNIT, MESSAGE_KEY_HA3_UNIT, MESSAGE_KEY_HA4_UNIT,
        MESSAGE_KEY_HA5_UNIT, MESSAGE_KEY_HA6_UNIT, MESSAGE_KEY_HA7_UNIT, MESSAGE_KEY_HA8_UNIT,
        MESSAGE_KEY_HA9_UNIT, MESSAGE_KEY_HA10_UNIT,
    };
    const uint32_t ha_min_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_MIN, MESSAGE_KEY_HA2_MIN, MESSAGE_KEY_HA3_MIN, MESSAGE_KEY_HA4_MIN,
        MESSAGE_KEY_HA5_MIN, MESSAGE_KEY_HA6_MIN, MESSAGE_KEY_HA7_MIN, MESSAGE_KEY_HA8_MIN,
        MESSAGE_KEY_HA9_MIN, MESSAGE_KEY_HA10_MIN,
    };
    const uint32_t ha_max_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_MAX, MESSAGE_KEY_HA2_MAX, MESSAGE_KEY_HA3_MAX, MESSAGE_KEY_HA4_MAX,
        MESSAGE_KEY_HA5_MAX, MESSAGE_KEY_HA6_MAX, MESSAGE_KEY_HA7_MAX, MESSAGE_KEY_HA8_MAX,
        MESSAGE_KEY_HA9_MAX, MESSAGE_KEY_HA10_MAX,
    };
    const uint32_t ha_style_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_STYLE, MESSAGE_KEY_HA2_STYLE, MESSAGE_KEY_HA3_STYLE, MESSAGE_KEY_HA4_STYLE,
        MESSAGE_KEY_HA5_STYLE, MESSAGE_KEY_HA6_STYLE, MESSAGE_KEY_HA7_STYLE, MESSAGE_KEY_HA8_STYLE,
        MESSAGE_KEY_HA9_STYLE, MESSAGE_KEY_HA10_STYLE,
    };
    const ChannelIndex ha_channels[NUM_HA_CHANNELS] = {
        CHANNEL_HA_1, CHANNEL_HA_2, CHANNEL_HA_3, CHANNEL_HA_4, CHANNEL_HA_5,
        CHANNEL_HA_6, CHANNEL_HA_7, CHANNEL_HA_8, CHANNEL_HA_9, CHANNEL_HA_10,
    };

    for (int i = 0; i < NUM_HA_CHANNELS; i++) {
        ChannelData *ch = &s_channels[ha_channels[i]];

        // HAn_KIND processed before HAn_VALUE so a same-message kind change
        // is already in effect by the time the value below is stored —
        // though since both numeric and binary just store into ch->value,
        // this ordering is a readability choice, not a correctness one.
        Tuple *kind_tuple = dict_find(iterator, ha_kind_keys[i]);
        if (kind_tuple && kind_tuple->type == TUPLE_CSTRING) {
            ChannelKind parsed_kind;
            if (parse_channel_kind(kind_tuple->value->cstring, &parsed_kind)) {
                ch->kind = parsed_kind;
                changed = true;
            }
        }

        Tuple *value_tuple = dict_find(iterator, ha_value_keys[i]);
        if (value_tuple && value_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->text, value_tuple->value->cstring, sizeof(ch->text) - 1);
            ch->text[sizeof(ch->text) - 1] = '\0';
            ch->kind = CHANNEL_KIND_TEXT;
            changed = true;
        } else if (value_tuple) {
            // Not a string, so a numeric (TUPLE_INT/TUPLE_UINT) wire value —
            // valid for both CHANNEL_KIND_NUMERIC and CHANNEL_KIND_BINARY,
            // which share the same int32 ch->value field. If HAn_KIND was
            // never sent (ch->kind still the CHANNEL_KIND_TEXT default from
            // init_channels()), assume NUMERIC rather than leaving the
            // channel showing nothing for a forgetful integration.
            ch->value = value_tuple->value->int32;
            if (ch->kind == CHANNEL_KIND_TEXT) {
                ch->kind = CHANNEL_KIND_NUMERIC;
            }
            changed = true;
        }

        Tuple *unit_tuple = dict_find(iterator, ha_unit_keys[i]);
        if (unit_tuple && unit_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->unit, unit_tuple->value->cstring, sizeof(ch->unit) - 1);
            ch->unit[sizeof(ch->unit) - 1] = '\0';
            changed = true;
        }

        // min/max only take effect together, in the same message — a range
        // is metadata sent once at channel setup, not something that
        // changes one bound at a time, so there's no need to track a
        // separate "min received but not max yet" state across messages.
        Tuple *min_tuple = dict_find(iterator, ha_min_keys[i]);
        Tuple *max_tuple = dict_find(iterator, ha_max_keys[i]);
        if (min_tuple && max_tuple) {
            ch->min = min_tuple->value->int32;
            ch->max = max_tuple->value->int32;
            ch->has_range = true;
            changed = true;
        }

        Tuple *style_tuple = dict_find(iterator, ha_style_keys[i]);
        if (style_tuple && style_tuple->type == TUPLE_CSTRING) {
            ChannelStyle parsed_style;
            if (parse_channel_style(style_tuple->value->cstring, &parsed_style)) {
                ch->style = parsed_style;
                changed = true;
            }
        }

        Tuple *label_tuple = dict_find(iterator, ha_label_keys[i]);
        if (label_tuple && label_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->label, label_tuple->value->cstring, sizeof(ch->label) - 1);
            ch->label[sizeof(ch->label) - 1] = '\0';
            changed = true;
        }

        Tuple *on_color_tuple = dict_find(iterator, ha_on_color_keys[i]);
        if (on_color_tuple && on_color_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->on_color, on_color_tuple->value->cstring, sizeof(ch->on_color) - 1);
            ch->on_color[sizeof(ch->on_color) - 1] = '\0';
            changed = true;
        }

        Tuple *off_color_tuple = dict_find(iterator, ha_off_color_keys[i]);
        if (off_color_tuple && off_color_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->off_color, off_color_tuple->value->cstring, sizeof(ch->off_color) - 1);
            ch->off_color[sizeof(ch->off_color) - 1] = '\0';
            changed = true;
        }

        Tuple *hide_when_tuple = dict_find(iterator, ha_hide_when_keys[i]);
        if (hide_when_tuple && hide_when_tuple->type == TUPLE_CSTRING) {
            ch->hide_when = parse_hide_when(hide_when_tuple->value->cstring);
            changed = true;
        }

        Tuple *bg_color_tuple = dict_find(iterator, ha_bg_color_keys[i]);
        if (bg_color_tuple && bg_color_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->bg_color, bg_color_tuple->value->cstring, sizeof(ch->bg_color) - 1);
            ch->bg_color[sizeof(ch->bg_color) - 1] = '\0';
            changed = true;
        }

        Tuple *value_color_tuple = dict_find(iterator, ha_value_color_keys[i]);
        if (value_color_tuple && value_color_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->value_color, value_color_tuple->value->cstring, sizeof(ch->value_color) - 1);
            ch->value_color[sizeof(ch->value_color) - 1] = '\0';
            changed = true;
        }

        Tuple *label_color_tuple = dict_find(iterator, ha_label_color_keys[i]);
        if (label_color_tuple && label_color_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->label_color, label_color_tuple->value->cstring, sizeof(ch->label_color) - 1);
            ch->label_color[sizeof(ch->label_color) - 1] = '\0';
            changed = true;
        }
    }

    return changed;
}

// Applies TIME_BG_COLOR..HEART_RATE_LABEL_COLOR (39 keys, 3 per local
// channel) for the 13 local channels. Split out of inbox_received_callback()
// for the same readability reason as apply_ha_channel_updates() above.
static bool apply_local_channel_colors(DictionaryIterator *iterator) {
    bool changed = false;

    const uint32_t local_bg_color_keys[NUM_LOCAL_COLOR_CHANNELS] = {
        MESSAGE_KEY_TIME_BG_COLOR, MESSAGE_KEY_DATE_BG_COLOR, MESSAGE_KEY_BATTERY_BG_COLOR, MESSAGE_KEY_STEPS_BG_COLOR,
        MESSAGE_KEY_CHARGE_BG_COLOR, MESSAGE_KEY_CONN_BG_COLOR, MESSAGE_KEY_ACTIVE_MINUTES_BG_COLOR, MESSAGE_KEY_DISTANCE_BG_COLOR,
        MESSAGE_KEY_ACTIVE_KCAL_BG_COLOR, MESSAGE_KEY_RESTING_KCAL_BG_COLOR, MESSAGE_KEY_SLEEP_MINUTES_BG_COLOR,
        MESSAGE_KEY_SLEEP_RESTFUL_MINUTES_BG_COLOR, MESSAGE_KEY_HEART_RATE_BG_COLOR,
    };
    const uint32_t local_value_color_keys[NUM_LOCAL_COLOR_CHANNELS] = {
        MESSAGE_KEY_TIME_VALUE_COLOR, MESSAGE_KEY_DATE_VALUE_COLOR, MESSAGE_KEY_BATTERY_VALUE_COLOR, MESSAGE_KEY_STEPS_VALUE_COLOR,
        MESSAGE_KEY_CHARGE_VALUE_COLOR, MESSAGE_KEY_CONN_VALUE_COLOR, MESSAGE_KEY_ACTIVE_MINUTES_VALUE_COLOR, MESSAGE_KEY_DISTANCE_VALUE_COLOR,
        MESSAGE_KEY_ACTIVE_KCAL_VALUE_COLOR, MESSAGE_KEY_RESTING_KCAL_VALUE_COLOR, MESSAGE_KEY_SLEEP_MINUTES_VALUE_COLOR,
        MESSAGE_KEY_SLEEP_RESTFUL_MINUTES_VALUE_COLOR, MESSAGE_KEY_HEART_RATE_VALUE_COLOR,
    };
    const uint32_t local_label_color_keys[NUM_LOCAL_COLOR_CHANNELS] = {
        MESSAGE_KEY_TIME_LABEL_COLOR, MESSAGE_KEY_DATE_LABEL_COLOR, MESSAGE_KEY_BATTERY_LABEL_COLOR, MESSAGE_KEY_STEPS_LABEL_COLOR,
        MESSAGE_KEY_CHARGE_LABEL_COLOR, MESSAGE_KEY_CONN_LABEL_COLOR, MESSAGE_KEY_ACTIVE_MINUTES_LABEL_COLOR, MESSAGE_KEY_DISTANCE_LABEL_COLOR,
        MESSAGE_KEY_ACTIVE_KCAL_LABEL_COLOR, MESSAGE_KEY_RESTING_KCAL_LABEL_COLOR, MESSAGE_KEY_SLEEP_MINUTES_LABEL_COLOR,
        MESSAGE_KEY_SLEEP_RESTFUL_MINUTES_LABEL_COLOR, MESSAGE_KEY_HEART_RATE_LABEL_COLOR,
    };

    for (int i = 0; i < NUM_LOCAL_COLOR_CHANNELS; i++) {
        ChannelData *ch = &s_channels[local_color_channels[i]];
        int bg_key = PERSIST_KEY_LOCAL_COLOR_BASE + i * 3 + 0;
        int value_key = PERSIST_KEY_LOCAL_COLOR_BASE + i * 3 + 1;
        int label_key = PERSIST_KEY_LOCAL_COLOR_BASE + i * 3 + 2;

        Tuple *bg_tuple = dict_find(iterator, local_bg_color_keys[i]);
        if (bg_tuple && bg_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->bg_color, bg_tuple->value->cstring, sizeof(ch->bg_color) - 1);
            ch->bg_color[sizeof(ch->bg_color) - 1] = '\0';
            persist_write_string(bg_key, ch->bg_color);
            changed = true;
        }

        Tuple *value_tuple = dict_find(iterator, local_value_color_keys[i]);
        if (value_tuple && value_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->value_color, value_tuple->value->cstring, sizeof(ch->value_color) - 1);
            ch->value_color[sizeof(ch->value_color) - 1] = '\0';
            persist_write_string(value_key, ch->value_color);
            changed = true;
        }

        Tuple *label_tuple = dict_find(iterator, local_label_color_keys[i]);
        if (label_tuple && label_tuple->type == TUPLE_CSTRING) {
            strncpy(ch->label_color, label_tuple->value->cstring, sizeof(ch->label_color) - 1);
            ch->label_color[sizeof(ch->label_color) - 1] = '\0';
            persist_write_string(label_key, ch->label_color);
            changed = true;
        }
    }

    return changed;
}

void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "inbox message dropped: reason=%d", (int)reason);
}

void inbox_received_callback(DictionaryIterator *iterator, void *context) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "inbox message received");
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
    // redraws the affected slots — never a layout change on their own. See
    // apply_ha_channel_updates() above for why this is its own function
    // rather than inline here.
    bool value_changed = apply_ha_channel_updates(iterator);

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

    // General-purpose background/value/label colors for the 13 local
    // channels — Clay-only; HA channels get the equivalent fields directly
    // in apply_ha_channel_updates() above. Doesn't affect slot rects/count,
    // so redraw-only like the dot config. See apply_local_channel_colors()
    // above for why this is its own function rather than inline here.
    bool color_config_changed = apply_local_channel_colors(iterator);

    APP_LOG(APP_LOG_LEVEL_DEBUG, "flags: layout=%d value=%d reporting=%d dot=%d color=%d",
            layout_changed, value_changed, reporting_config_changed, dot_config_changed, color_config_changed);

    if (!layout_changed && !value_changed && !reporting_config_changed && !dot_config_changed
        && !color_config_changed) {
        return;
    }

    if (layout_changed) {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "rebuilding layout");
        Layer *window_layer = window_get_root_layer(s_main_window);
        GRect bounds = layer_get_bounds(window_layer);
        destroy_active_layout();
        load_active_layout(window_layer, bounds);

        update_time();
        update_battery();
        update_steps();
        update_health_channels();
        APP_LOG(APP_LOG_LEVEL_DEBUG, "layout rebuilt");
    } else if (value_changed || dot_config_changed || color_config_changed) {
        mark_all_slots_dirty();
    }

    // Report promptly on a toggle change instead of waiting up to a minute
    // for the next tick — this is exactly the "let HA know now" signal the
    // disabled-groups list exists for.
    if (reporting_config_changed) {
        // Return from the inbox callback before starting an outbox transaction,
        // so the phone-side config acknowledgement can complete first.
        app_timer_register(1, deferred_status_report_callback, NULL);
    }
}
