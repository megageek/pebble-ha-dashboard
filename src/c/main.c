#include <pebble.h>

#define MAX_SLOTS 8

typedef enum {
    CHANNEL_TIME,
    CHANNEL_DATE,
    CHANNEL_BATTERY,
    CHANNEL_STEPS,
    CHANNEL_HA_PLACEHOLDER,
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
    ChannelIndex channel;
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

static void init_channels(void) {
    s_channels[CHANNEL_TIME] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "TIME",
    };
    s_channels[CHANNEL_DATE] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "DATE",
    };
    s_channels[CHANNEL_BATTERY] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_BAR, .label = "BATT",
        .min = 0, .max = 100, .has_range = true,
    };
    s_channels[CHANNEL_STEPS] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "STEPS",
    };
    // Stands in for a future Home Assistant-pushed channel until the pkjs bridge exists.
    s_channels[CHANNEL_HA_PLACEHOLDER] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "CH3", .text = "--",
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
    const ChannelIndex stat_channels[3] = { CHANNEL_BATTERY, CHANNEL_STEPS, CHANNEL_HA_PLACEHOLDER };

    for (int i = 0; i < num_stats; i++) {
        int row_top = stats_top + i * row_height;
        out[n++] = (SlotSpec) {
            GRect(0, row_top, bounds.size.w, row_height), SLOT_SIZE_SMALL, stat_channels[i],
        };
    }

    return n;
}

static void mark_all_slots_dirty(void) {
    for (int i = 0; i < s_slot_count; i++) {
        layer_mark_dirty(s_slot_layers[i]);
    }
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
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    update_time();
    update_steps();
}

static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    window_set_background_color(window, GColorBlack);

    init_channels();

    SlotSpec specs[MAX_SLOTS];
    s_slot_count = build_template_0(bounds, specs);

    for (int i = 0; i < s_slot_count; i++) {
        Layer *slot_layer = layer_create_with_data(specs[i].rect, sizeof(SlotRenderInfo));
        SlotRenderInfo *info = layer_get_data(slot_layer);
        info->channel_index = specs[i].channel;
        info->size_class = specs[i].size_class;
        layer_set_update_proc(slot_layer, slot_update_proc);
        layer_add_child(window_layer, slot_layer);
        s_slot_layers[i] = slot_layer;
    }

    // Divider between the date slot and the stat rows below it.
    int separator_top = specs[1].rect.origin.y + specs[1].rect.size.h + 6;
    s_separator_layer = layer_create(GRect(10, separator_top, bounds.size.w - 20, 1));
    layer_set_update_proc(s_separator_layer, separator_update_proc);
    layer_add_child(window_layer, s_separator_layer);

    update_time();
    update_battery();
    update_steps();
}

static void main_window_unload(Window *window) {
    for (int i = 0; i < s_slot_count; i++) {
        layer_destroy(s_slot_layers[i]);
    }
    layer_destroy(s_separator_layer);
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
