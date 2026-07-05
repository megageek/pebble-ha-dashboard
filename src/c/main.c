#include <pebble.h>

#define MAX_SLOTS 8
#define NUM_CONFIGURABLE_SLOTS 5  // must match the SLOT_n_CHANNEL messageKeys in package.json
#define NUM_HA_CHANNELS 3  // must match the HAn_VALUE/HAn_LABEL messageKeys in package.json

#define PERSIST_KEY_TEMPLATE 100
#define PERSIST_KEY_SLOT_BASE 101  // slot i -> PERSIST_KEY_SLOT_BASE + i

typedef enum {
    CHANNEL_TIME,
    CHANNEL_DATE,
    CHANNEL_BATTERY,
    CHANNEL_STEPS,
    CHANNEL_HA_1,
    CHANNEL_HA_2,
    CHANNEL_HA_3,
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
} SlotRenderInfo;

static Window *s_main_window;
static Layer *s_separator_layer;
static Layer *s_slot_layers[MAX_SLOTS];
static int s_slot_count = 0;

static ChannelData s_channels[NUM_CHANNELS];

static int s_active_template = 0;
// -1 means "use the template's default channel for this slot".
static int s_slot_channel_override[NUM_CONFIGURABLE_SLOTS];

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
    s_channels[CHANNEL_HA_1] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA1", .text = "--",
    };
    s_channels[CHANNEL_HA_2] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA2", .text = "--",
    };
    s_channels[CHANNEL_HA_3] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA3", .text = "--",
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

static void slot_update_proc(Layer *layer, GContext *ctx) {
    SlotRenderInfo *info = layer_get_data(layer);
    ChannelData *ch = &s_channels[info->channel_index];
    GRect bounds = layer_get_bounds(layer);

    switch (info->size_class) {
        case SLOT_SIZE_HERO:
            draw_channel_text_aligned(ctx, bounds, ch, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD),
                                       GTextAlignmentCenter, GColorWhite);
            break;
        case SLOT_SIZE_MEDIUM:
            draw_channel_text_aligned(ctx, bounds, ch, fonts_get_system_font(FONT_KEY_GOTHIC_24),
                                       GTextAlignmentCenter, GColorLightGray);
            break;
        case SLOT_SIZE_SMALL:
        default:
            draw_channel_small_row(ctx, bounds, ch);
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
// a new wake-up source, except the heart rate sample-period request, which
// has a real, acknowledged battery cost.
// ---------------------------------------------------------------------------

static void write_health_metric_if_available(DictionaryIterator *iter, uint32_t key,
                                              HealthMetric metric, time_t start, time_t end) {
    HealthServiceAccessibilityMask mask = health_service_metric_accessible(metric, start, end);
    if (mask & HealthServiceAccessibilityMaskAvailable) {
        HealthValue value = health_service_sum(metric, start, end);
        dict_write_int32(iter, key, (int32_t)value);
    }
}

static void send_status_report(void) {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
        return;  // outbox busy; the next periodic call will try again
    }

    BatteryChargeState battery = battery_state_service_peek();
    dict_write_int32(iter, MESSAGE_KEY_REPORT_BATTERY_PERCENT, battery.charge_percent);
    dict_write_int32(iter, MESSAGE_KEY_REPORT_BATTERY_CHARGING, battery.is_charging ? 1 : 0);
    dict_write_int32(iter, MESSAGE_KEY_REPORT_CONNECTED,
                      connection_service_peek_pebble_app_connection() ? 1 : 0);

    time_t day_start = time_start_of_today();
    time_t now = time(NULL);

    write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_STEPS, HealthMetricStepCount, day_start, now);
    write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_ACTIVE_SECONDS, HealthMetricActiveSeconds, day_start, now);
    write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_DISTANCE_METERS, HealthMetricWalkedDistanceMeters, day_start, now);
    write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_ACTIVE_KCAL, HealthMetricActiveKCalories, day_start, now);
    write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_RESTING_KCAL, HealthMetricRestingKCalories, day_start, now);
    write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_SLEEP_SECONDS, HealthMetricSleepSeconds, day_start, now);
    write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_SLEEP_RESTFUL_SECONDS, HealthMetricSleepRestfulSeconds, day_start, now);

    // Heart rate is an instantaneous reading, not a daily sum — accessibility
    // is checked for "right now", and health_service_peek_current_value()
    // (not _sum_today()) returns the latest sample.
    HealthServiceAccessibilityMask hr_mask = health_service_metric_accessible(HealthMetricHeartRateBPM, now, now);
    if (hr_mask & HealthServiceAccessibilityMaskAvailable) {
        HealthValue hr = health_service_peek_current_value(HealthMetricHeartRateBPM);
        dict_write_int32(iter, MESSAGE_KEY_REPORT_HEART_RATE_BPM, (int32_t)hr);
    }

    app_message_outbox_send();
}

// Requesting fresher heart rate samples has a real battery cost, so the
// elevated sample rate HA asked us to keep to a minute (matching our
// existing tick cadence) rather than anything more aggressive — and it
// expires on its own, so it's renewed every tick rather than requested once.
static void request_heart_rate_updates(void) {
    health_service_set_heart_rate_sample_period(60);
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
    mark_all_slots_dirty();
}

static void update_steps(void) {
    time_t start = time_start_of_today();
    time_t end = time(NULL);
    HealthServiceAccessibilityMask mask =
        health_service_metric_accessible(HealthMetricStepCount, start, end);

    if (mask & HealthServiceAccessibilityMaskAvailable) {
        s_channels[CHANNEL_STEPS].kind = CHANNEL_KIND_NUMERIC;
        s_channels[CHANNEL_STEPS].value = (int32_t)health_service_sum_today(HealthMetricStepCount);
    } else {
        s_channels[CHANNEL_STEPS].kind = CHANNEL_KIND_TEXT;
        strncpy(s_channels[CHANNEL_STEPS].text, "N/A", sizeof(s_channels[CHANNEL_STEPS].text));
    }

    mark_all_slots_dirty();
}

static void battery_callback(BatteryChargeState state) {
    s_channels[CHANNEL_BATTERY].value = state.charge_percent;
    mark_all_slots_dirty();
    send_status_report();
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    update_time();
    update_steps();
    request_heart_rate_updates();
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
        MESSAGE_KEY_SLOT_3_CHANNEL, MESSAGE_KEY_SLOT_4_CHANNEL,
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
        MESSAGE_KEY_HA1_VALUE, MESSAGE_KEY_HA2_VALUE, MESSAGE_KEY_HA3_VALUE,
    };
    const uint32_t ha_label_keys[NUM_HA_CHANNELS] = {
        MESSAGE_KEY_HA1_LABEL, MESSAGE_KEY_HA2_LABEL, MESSAGE_KEY_HA3_LABEL,
    };
    const ChannelIndex ha_channels[NUM_HA_CHANNELS] = { CHANNEL_HA_1, CHANNEL_HA_2, CHANNEL_HA_3 };

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
    }

    if (!layout_changed && !value_changed) {
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
    } else {
        mark_all_slots_dirty();
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

    request_heart_rate_updates();
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
