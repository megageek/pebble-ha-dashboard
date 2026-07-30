#include "rendering.h"
#include "channels.h"
#include "status_dots.h"
#include <stdio.h>

#define DOT_AREA_MARGIN 8  // gap between the dot cluster and the slot's normal content

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

// `default_color` is whatever the caller's size class would normally use;
// ch->value_color (Clay/HA-configurable, see ChannelData) overrides it when
// set, falling back to default_color when empty so an unconfigured channel
// renders exactly as before this option existed.
static void draw_channel_text_aligned(GContext *ctx, GRect bounds, ChannelData *ch,
                                       GFont font, GTextAlignment alignment, GColor default_color) {
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

    GColor color = ch->value_color[0] != '\0' ? color_from_name(ch->value_color) : default_color;
    graphics_context_set_text_color(ctx, color);
    graphics_draw_text(ctx, text, font, bounds, GTextOverflowModeTrailingEllipsis, alignment, NULL);
}

static void draw_channel_small_row(GContext *ctx, GRect bounds, ChannelData *ch) {
    int half = bounds.size.w / 2;
    GRect label_rect = GRect(bounds.origin.x + 14, bounds.origin.y, half - 14, bounds.size.h);
    GRect value_rect = GRect(bounds.origin.x + half, bounds.origin.y, half - 14, bounds.size.h);

    GColor label_color = ch->label_color[0] != '\0' ? color_from_name(ch->label_color) : GColorLightGray;
    graphics_context_set_text_color(ctx, label_color);
    graphics_draw_text(ctx, ch->label, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), label_rect,
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

    if (ch->style == CHANNEL_STYLE_BAR && ch->kind == CHANNEL_KIND_NUMERIC && ch->has_range) {
        draw_channel_bar(ctx, value_rect, ch);
    } else {
        draw_channel_text_aligned(ctx, value_rect, ch, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                                   GTextAlignmentRight, GColorWhite);
    }
}

void separator_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(0, 0), GPoint(bounds.size.w, 0));
}

void slot_update_proc(Layer *layer, GContext *ctx) {
    SlotRenderInfo *info = layer_get_data(layer);
    GRect bounds = layer_get_bounds(layer);
    ChannelData *ch = &s_channels[info->channel_index];

    // Empty bg_color (the default) means "no override" — leave the
    // window's own black background showing through, exactly as before
    // this option existed. Filled first, under everything else in the
    // slot (dots included), since it's a whole-slot background.
    if (ch->bg_color[0] != '\0') {
        graphics_context_set_fill_color(ctx, color_from_name(ch->bg_color));
        graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    }

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
