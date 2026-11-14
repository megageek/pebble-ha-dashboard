#include "rendering.h"
#include "channels.h"
#include "status_dots.h"
#include <stdio.h>

#define DOT_AREA_MARGIN 8  // gap between the dot cluster and the slot's normal content

// outline_color/fill_color let callers match their own backdrop — the
// SLOT_SIZE_SMALL row call site (on the app's black background) uses
// light-gray/white as before; the card call site (on a white card) needs
// black/black instead, since white-on-white would be invisible.
static void draw_channel_bar(GContext *ctx, GRect bounds, ChannelData *ch,
                              GColor outline_color, GColor fill_color) {
    const int bar_height = 12;
    GRect bar_frame = GRect(bounds.origin.x, bounds.origin.y + (bounds.size.h - bar_height) / 2,
                             bounds.size.w, bar_height);

    graphics_context_set_stroke_color(ctx, outline_color);
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
        graphics_context_set_fill_color(ctx, fill_color);
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

    // graphics_draw_text() bottom-anchors text within a box taller than the
    // text needs (confirmed by rendering temporary on-screen markers at a
    // box's actual top/bottom edges during development), so this stays a
    // plain pass-through — no vertical adjustment applied here. A vertical
    // nudge WAS tried at this shared level (moving the box up a fixed
    // amount whenever there was measured slack), but it broke
    // SLOT_SIZE_SMALL's label/value alignment: the label is drawn directly
    // via a separate graphics_draw_text() call in draw_channel_small_row()
    // that never got nudged, while the value (going through this function)
    // did — same row, same bounds.origin.y, now visibly misaligned. Any
    // vertical positioning fix needs to be scoped to the specific caller
    // that needs it (see SLOT_SIZE_CARD's draw_channel_card(), which
    // adjusts its own value_rect directly instead of relying on this
    // function to do it) rather than applied blindly to every caller here.
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
        draw_channel_bar(ctx, value_rect, ch, GColorLightGray, GColorWhite);
    } else {
        draw_channel_text_aligned(ctx, value_rect, ch, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                                   GTextAlignmentRight, GColorWhite);
    }
}

#define CARD_RADIUS 6       // <= graphics_fill_rect()'s 8px cap
#define CARD_PADDING 4
#define CARD_LABEL_HEIGHT 16  // fits FONT_KEY_GOTHIC_14_BOLD (the label font) with a little margin
// Value font sized against measurements taken via graphics_text_layout_
// get_content_size() (logged during development, not guessed):
// FONT_KEY_GOTHIC_18_BOLD needs ~18px. A card's value_rect must be at
// least that tall or the glyph gets clipped — graphics_draw_text() does
// NOT shrink/scale text to fit a too-small box, it just cuts off whatever
// doesn't fit. One standard tier covers every stat card regardless of
// width (confirmed: GOTHIC_18_BOLD fits both the ~92px-wide top cards and
// the ~60px-wide day/date/year cards without truncating any current label
// text) — only a sufficiently TALL card (currently just the clock) steps
// up to the hero tier, since a digital clock is the visual centerpiece of
// this template and reads poorly at stat-card size. Keyed off height, not
// the channel itself, so any future template's tall card gets this for
// free too. Hero cards also skip the label row entirely (see
// draw_channel_card()) so the value can fill the whole cell, margin only.
#define CARD_HERO_HEIGHT_THRESHOLD 50
// Empirically tuned (see draw_channel_card()'s hero branch) against
// on-screen markers at value_rect's actual top/bottom edges — not derived
// from font metrics, which don't predict where LECO_42_NUMBERS' visual ink
// actually lands within a box taller than the text needs.
#define CARD_HERO_VERTICAL_NUDGE 4
// Standard-tier cards are text (or a bar), bottom-anchored like everything
// else drawn via graphics_draw_text() — moves the value area's bottom edge
// up by this much (shrinking value_rect from the bottom, not translating
// it) so the value doesn't sit flush against the card's own bottom edge.
// Scoped to draw_channel_card() only, unlike an earlier attempt at this
// via draw_channel_text_aligned() itself, which broke SLOT_SIZE_SMALL's
// label/value alignment in templates 0/1 (see that function's comment) —
// build_template_2()'s row heights budget extra room specifically so this
// margin doesn't clip GOTHIC_18_BOLD's ~18px need.
#define CARD_VALUE_BOTTOM_MARGIN 4

// A bordered white card: black outline (1px stroke, since no rounded-rect
// fill primitive in this SDK exceeds an 8px radius, see graphics_fill_rect()'s
// doc comment — a thin stroke via graphics_draw_round_rect() reads cleanly
// at this size regardless), a small label at the top, and a big centered
// value below it (or an enlarged bar for style==BAR channels, e.g.
// CHANNEL_BATTERY). Font size adapts to the slot's actual height rather
// than needing a second "small card" SlotSizeClass. bg_color/dot colors
// have no visible effect here — the card always self-fills white/black,
// see the comment on SlotSizeClass in types.h.
static void draw_channel_card(GContext *ctx, GRect bounds, ChannelData *ch) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, bounds, CARD_RADIUS, GCornersAll);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_round_rect(ctx, bounds, CARD_RADIUS);

    bool hero = bounds.size.h >= CARD_HERO_HEIGHT_THRESHOLD;
    GRect value_rect;
    if (hero) {
        // No label row — the hero card (currently just the clock) fills
        // its whole cell, margin only, matching the reference screenshot
        // (which has no visible "TIME" label on its clock either).
        value_rect = GRect(bounds.origin.x + CARD_PADDING, bounds.origin.y + CARD_PADDING,
                            bounds.size.w - 2 * CARD_PADDING, bounds.size.h - 2 * CARD_PADDING);
    } else {
        GRect label_rect = GRect(bounds.origin.x + CARD_PADDING, bounds.origin.y + CARD_PADDING / 2,
                                  bounds.size.w - 2 * CARD_PADDING, CARD_LABEL_HEIGHT);
        // Half-tone (GColorLightGray, #AAAAAA) rather than solid black — the
        // same muted-vs-value-text hierarchy SLOT_SIZE_SMALL's label already
        // has (light gray label, white/black value), just inverted for a
        // white card. GColorBlack would fully match the value's weight and
        // lose that hierarchy.
        graphics_context_set_text_color(ctx, GColorLightGray);
        graphics_draw_text(ctx, ch->label, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), label_rect,
                            GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
        value_rect = GRect(bounds.origin.x + CARD_PADDING, bounds.origin.y + CARD_PADDING / 2 + CARD_LABEL_HEIGHT,
                            bounds.size.w - 2 * CARD_PADDING,
                            bounds.size.h - CARD_PADDING / 2 - CARD_LABEL_HEIGHT - CARD_PADDING
                                - CARD_VALUE_BOTTOM_MARGIN);
    }

    if (ch->style == CHANNEL_STYLE_BAR && ch->kind == CHANNEL_KIND_NUMERIC && ch->has_range) {
        draw_channel_bar(ctx, value_rect, ch, GColorBlack, GColorBlack);
    } else if (hero) {
        // LECO_42_NUMBERS — Pebble's dedicated digital-clock digit font
        // (digits/colon only, not full-charset — fine for CHANNEL_TIME, but
        // a future non-numeric hero card would need a different font).
        //
        // Drawn directly here rather than via draw_channel_text_aligned():
        // confirmed by rendering temporary on-screen markers at value_rect's
        // top/bottom edges that graphics_draw_text() bottom-anchors text
        // within a box taller than the text needs (not top-anchored, as
        // draw_channel_text_aligned()'s own centering logic assumes) — a
        // box "sized to fit with a little margin" left the text sitting
        // flush against the bottom of value_rect with all the slack at the
        // top. CARD_HERO_VERTICAL_NUDGE (translating the whole box up,
        // same height, not shrinking/growing it) was tuned against those
        // markers directly, not derived from graphics_text_layout_get_
        // content_size() — that API's reported height doesn't correspond
        // to where this font's visual ink actually anchors.
        GRect nudged = GRect(value_rect.origin.x, value_rect.origin.y - CARD_HERO_VERTICAL_NUDGE,
                              value_rect.size.w, value_rect.size.h);
        graphics_context_set_text_color(ctx, GColorBlack);
        graphics_draw_text(ctx, ch->text, fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS), nudged,
                            GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    } else {
        draw_channel_text_aligned(ctx, value_rect, ch, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                                   GTextAlignmentCenter, GColorBlack);
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
        case SLOT_SIZE_CARD:
            draw_channel_card(ctx, content_bounds, ch);
            break;
        case SLOT_SIZE_SMALL:
        default:
            draw_channel_small_row(ctx, content_bounds, ch);
            break;
    }
}
