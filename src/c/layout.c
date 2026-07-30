#include "layout.h"
#include "rendering.h"

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

static const TemplateBuilder s_templates[] = { build_template_0, build_template_1 };
const int NUM_TEMPLATES = (int)(sizeof(s_templates) / sizeof(s_templates[0]));

Window *s_main_window;
int s_active_template = 0;
int s_slot_channel_override[NUM_CONFIGURABLE_SLOTS];

static Layer *s_separator_layer;
static Layer *s_slot_layers[MAX_SLOTS];
static int s_slot_count = 0;

void mark_all_slots_dirty(void) {
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

void load_active_layout(Layer *window_layer, GRect bounds) {
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

void destroy_active_layout(void) {
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
