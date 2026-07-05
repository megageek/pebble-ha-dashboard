#include <pebble.h>

#define NUM_CHANNELS 3

static Window *s_main_window;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static Layer *s_separator_layer;
static TextLayer *s_channel_label_layers[NUM_CHANNELS];
static TextLayer *s_channel_value_layers[NUM_CHANNELS];

static char s_time_buffer[8];
static char s_date_buffer[16];

static const char *s_channel_labels[NUM_CHANNELS] = { "CH1", "CH2", "CH3" };
static const char *s_channel_placeholder = "--";

static void separator_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(0, 0), GPoint(bounds.size.w, 0));
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

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    update_time();
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

        TextLayer *value_layer = text_layer_create(
            GRect(bounds.size.w / 2, row_top, value_width, row_height));
        text_layer_set_background_color(value_layer, GColorClear);
        text_layer_set_text_color(value_layer, GColorWhite);
        text_layer_set_font(value_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
        text_layer_set_text_alignment(value_layer, GTextAlignmentRight);
        text_layer_set_text(value_layer, s_channel_placeholder);
        layer_add_child(window_layer, text_layer_get_layer(value_layer));
        s_channel_value_layers[i] = value_layer;
    }

    update_time();
}

static void main_window_unload(Window *window) {
    text_layer_destroy(s_time_layer);
    text_layer_destroy(s_date_layer);
    layer_destroy(s_separator_layer);

    for (int i = 0; i < NUM_CHANNELS; i++) {
        text_layer_destroy(s_channel_label_layers[i]);
        text_layer_destroy(s_channel_value_layers[i]);
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
}

static void deinit(void) {
    tick_timer_service_unsubscribe();
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
    return 0;
}
