#include "status_dots.h"
#include "channels.h"
#include <string.h>

int s_dot_group_slot = -1;
int s_dot_channels[MAX_DOTS];

const int DOT_DIAMETERS[NUM_DOT_SIZES] = { 6, 9, 12 };
const int DOT_SPACINGS[NUM_DOT_SIZES] = { 5, 6, 8 };
int s_dot_size = DOT_SIZE_LARGE;

int count_assigned_dots(void) {
    int count = 0;
    for (int i = 0; i < MAX_DOTS; i++) {
        if (s_dot_channels[i] >= 0) {
            count++;
        }
    }
    return count;
}

void draw_dot_group(GContext *ctx, GRect bounds) {
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

bool apply_dot_color_config(DictionaryIterator *iterator, ChannelIndex channel,
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
