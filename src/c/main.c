#include <pebble.h>

#define NUM_CHANNELS 3

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
    int32_t value;      // numeric value, or 0/1 for binary
    int32_t min;
    int32_t max;
    bool has_range;
    char unit[8];       // optional unit suffix for numeric channels, e.g. "%"
    char text[16];       // used when kind == CHANNEL_KIND_TEXT
} ChannelData;

static Window *s_main_window;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static Layer *s_separator_layer;
static TextLayer *s_channel_label_layers[NUM_CHANNELS];
static Layer *s_channel_value_layers[NUM_CHANNELS];

static char s_time_buffer[8];
static char s_date_buffer[16];

static ChannelData s_channels[NUM_CHANNELS];

// Slots 0 and 1 are always-available local channels (battery, steps).
// Slot 2 stands in for a Home Assistant-pushed channel until the pkjs
// bridge exists.
static const char *s_channel_labels[NUM_CHANNELS] = { "BATT", "STEPS", "CH3" };

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

static void draw_channel_text(GContext *ctx, GRect bounds, ChannelData *ch) {
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

    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_18), bounds,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
}

static void channel_value_update_proc(Layer *layer, GContext *ctx) {
    int idx = *(int *)layer_get_data(layer);
    ChannelData *ch = &s_channels[idx];
    GRect bounds = layer_get_bounds(layer);

    if (ch->style == CHANNEL_STYLE_BAR && ch->kind == CHANNEL_KIND_NUMERIC && ch->has_range) {
        draw_channel_bar(ctx, bounds, ch);
    } else {
        draw_channel_text(ctx, bounds, ch);
    }
}

static void update_time(void) {
    time_t temp = time(NULL);
    struct tm *tick_time = localtime(&temp);
    if (!tick_time) {
        return;
    }

    strftime(s_time_buffer, sizeof(s_time_buffer),
              clock_is_24h_style() ? "%H:%M" : "%I:%M", tick_time);
    text_layer_set_text(s_time_layer, s_time_buffer);

    strftime(s_date_buffer, sizeof(s_date_buffer), "%a %d %b", tick_time);
    text_layer_set_text(s_date_layer, s_date_buffer);
}

static void update_battery(void) {
    BatteryChargeState state = battery_state_service_peek();
    s_channels[0].value = state.charge_percent;
    if (s_channel_value_layers[0]) {
        layer_mark_dirty(s_channel_value_layers[0]);
    }
}

static void update_steps(void) {
    time_t start = time_start_of_today();
    time_t end = time(NULL);
    HealthServiceAccessibilityMask mask =
        health_service_metric_accessible(HealthMetricStepCount, start, end);

    if (mask & HealthServiceAccessibilityMaskAvailable) {
        s_channels[1].kind = CHANNEL_KIND_NUMERIC;
        s_channels[1].value = (int32_t)health_service_sum_today(HealthMetricStepCount);
    } else {
        s_channels[1].kind = CHANNEL_KIND_TEXT;
        strncpy(s_channels[1].text, "N/A", sizeof(s_channels[1].text));
    }

    if (s_channel_value_layers[1]) {
        layer_mark_dirty(s_channel_value_layers[1]);
    }
}

static void battery_callback(BatteryChargeState state) {
    s_channels[0].value = state.charge_percent;
    if (s_channel_value_layers[0]) {
        layer_mark_dirty(s_channel_value_layers[0]);
    }
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    update_time();
    update_steps();
}

static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    window_set_background_color(window, GColorBlack);

    // Time
    const int time_top = 24;
    const int time_height = 60;
    s_time_layer = text_layer_create(GRect(0, time_top, bounds.size.w, time_height));
    text_layer_set_background_color(s_time_layer, GColorClear);
    text_layer_set_text_color(s_time_layer, GColorWhite);
    text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
    text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
    layer_add_child(window_layer, text_layer_get_layer(s_time_layer));

    // Date
    const int date_top = time_top + time_height + 4;
    const int date_height = 28;
    s_date_layer = text_layer_create(GRect(0, date_top, bounds.size.w, date_height));
    text_layer_set_background_color(s_date_layer, GColorClear);
    text_layer_set_text_color(s_date_layer, GColorLightGray);
    text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));
    text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);
    layer_add_child(window_layer, text_layer_get_layer(s_date_layer));

    // Separator between clock and channels
    const int separator_top = date_top + date_height + 6;
    s_separator_layer = layer_create(GRect(10, separator_top, bounds.size.w - 20, 1));
    layer_set_update_proc(s_separator_layer, separator_update_proc);
    layer_add_child(window_layer, s_separator_layer);

    // Channel data: battery is numeric with a 0-100 range, rendered as a bar.
    // Steps is numeric with no fixed range (plain text). CH3 is a text
    // placeholder standing in for a future HA-pushed channel.
    s_channels[0] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_BAR,
        .value = 0, .min = 0, .max = 100, .has_range = true, .unit = "",
    };
    s_channels[1] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW,
        .value = 0, .has_range = false, .unit = "",
    };
    s_channels[2] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .text = "--",
    };

    // Channel rows fill the remaining space above the bottom margin
    const int channels_top = separator_top + 10;
    const int bottom_margin = 6;
    const int available = bounds.size.h - channels_top - bottom_margin;
    const int row_height = available / NUM_CHANNELS;
    const int label_width = bounds.size.w / 2 - 14;
    const int value_width = bounds.size.w / 2 - 14;

    for (int i = 0; i < NUM_CHANNELS; i++) {
        int row_top = channels_top + i * row_height;

        TextLayer *label_layer = text_layer_create(GRect(14, row_top, label_width, row_height));
        text_layer_set_background_color(label_layer, GColorClear);
        text_layer_set_text_color(label_layer, GColorLightGray);
        text_layer_set_font(label_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
        text_layer_set_text_alignment(label_layer, GTextAlignmentLeft);
        text_layer_set_text(label_layer, s_channel_labels[i]);
        layer_add_child(window_layer, text_layer_get_layer(label_layer));
        s_channel_label_layers[i] = label_layer;

        Layer *value_layer = layer_create_with_data(
            GRect(bounds.size.w / 2, row_top, value_width, row_height), sizeof(int));
        *(int *)layer_get_data(value_layer) = i;
        layer_set_update_proc(value_layer, channel_value_update_proc);
        layer_add_child(window_layer, value_layer);
        s_channel_value_layers[i] = value_layer;
    }

    update_time();
    update_battery();
    update_steps();
}

static void main_window_unload(Window *window) {
    text_layer_destroy(s_time_layer);
    text_layer_destroy(s_date_layer);
    layer_destroy(s_separator_layer);

    for (int i = 0; i < NUM_CHANNELS; i++) {
        text_layer_destroy(s_channel_label_layers[i]);
        layer_destroy(s_channel_value_layers[i]);
    }
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
