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

// Template 2: a bordered white-card dashboard on a light background (see
// s_template_bg_colors/active_template_bg_color() below) — two rows of
// half-width cards, two full-width cards, then a row of 3 small cards, all
// SLOT_SIZE_CARD. No separator line — see load_active_layout()'s
// needs_separator check, which skips it whenever every slot is a card.
static int build_template_2(GRect bounds, SlotSpec *out) {
    int n = 0;
    const int margin = 2;
    const int gap = 1;
    const int content_w = bounds.size.w - 2 * margin;

    // Row heights below are sized against real measurements (taken via
    // graphics_text_layout_get_content_size(), not guessed): a card's
    // value_rect must be at least ~18px tall for FONT_KEY_GOTHIC_18_BOLD
    // (the standard value font, see draw_channel_card() in rendering.c) or
    // ~42px for FONT_KEY_LECO_42_NUMBERS (the clock's hero font) — anything
    // shorter clips the glyph, since graphics_draw_text() doesn't shrink
    // text to fit. Standard-tier cards also budget CARD_VALUE_BOTTOM_MARGIN
    // on top of that minimum (draw_channel_card() shrinks value_rect's
    // bottom edge by that much so the value doesn't sit flush against the
    // card's own bottom edge) — 44px (= CARD_PADDING/2 + CARD_LABEL_HEIGHT +
    // 18 + CARD_PADDING + CARD_VALUE_BOTTOM_MARGIN) is the minimum for a
    // text-value standard-tier card; the bar-value one (battery) doesn't
    // need the margin's headroom (a 12px bar fits in far less than 18px),
    // so it stays at a tighter 38px; 50px for the hero-tier clock card.

    // Row 1: two half-width cards (temp/HA1, steps).
    const int row1_top = margin;
    const int row1_h = 44;
    const int half_w = (content_w - gap) / 2;
    out[n++] = (SlotSpec) {
        GRect(margin, row1_top, half_w, row1_h), SLOT_SIZE_CARD, CHANNEL_HA_1,
    };
    out[n++] = (SlotSpec) {
        GRect(margin + half_w + gap, row1_top, content_w - half_w - gap, row1_h), SLOT_SIZE_CARD, CHANNEL_STEPS,
    };

    // Row 2: two half-width cards (distance, HA2) — same split as row 1.
    const int row2_top = row1_top + row1_h + gap;
    const int row2_h = 44;
    out[n++] = (SlotSpec) {
        GRect(margin, row2_top, half_w, row2_h), SLOT_SIZE_CARD, CHANNEL_DISTANCE,
    };
    out[n++] = (SlotSpec) {
        GRect(margin + half_w + gap, row2_top, content_w - half_w - gap, row2_h), SLOT_SIZE_CARD, CHANNEL_HA_2,
    };

    // Row 3: full-width battery card (renders as a bar via existing style==BAR).
    const int row3_top = row2_top + row2_h + gap;
    const int row3_h = 38;
    out[n++] = (SlotSpec) {
        GRect(margin, row3_top, content_w, row3_h), SLOT_SIZE_CARD, CHANNEL_BATTERY,
    };

    // Row 4: full-width big clock card — tall enough to cross
    // CARD_HERO_HEIGHT_THRESHOLD in rendering.c, so it gets the larger
    // digital-clock-style font instead of the same size every other card uses.
    const int row4_top = row3_top + row3_h + gap;
    const int row4_h = 50;
    out[n++] = (SlotSpec) {
        GRect(margin, row4_top, content_w, row4_h), SLOT_SIZE_CARD, CHANNEL_TIME,
    };

    // Row 5: three small cards (day/date/year) — also text-value standard
    // tier, so it needs the same 44px-equivalent headroom as rows 1-2;
    // computed dynamically below rather than hardcoded like the others.
    const int row5_top = row4_top + row4_h + gap;
    const int bottom_margin = margin;
    const int row5_h = bounds.size.h - row5_top - bottom_margin;
    const int small_gap = 2;
    const int small_w = (content_w - 2 * small_gap) / 3;
    const ChannelIndex small_channels[3] = { CHANNEL_DAY_NAME, CHANNEL_DATE_SHORT, CHANNEL_YEAR };
    for (int i = 0; i < 3; i++) {
        int x = margin + i * (small_w + small_gap);
        int w = (i == 2) ? (margin + content_w - x) : small_w;  // last card absorbs rounding remainder
        out[n++] = (SlotSpec) {
            GRect(x, row5_top, w, row5_h), SLOT_SIZE_CARD, small_channels[i],
        };
    }

    return n;
}

static const TemplateBuilder s_templates[] = { build_template_0, build_template_1, build_template_2 };
const int NUM_TEMPLATES = (int)(sizeof(s_templates) / sizeof(s_templates[0]));

// Background color per template, indexed by s_active_template — templates
// 0/1 keep the original black background every slot assumes (no bg_color
// = black shows through, see rendering.c); template 2's white cards need a
// light page background instead, or black would bleed through the margins/
// gaps between cards.
static const GColor s_template_bg_colors[] = { GColorBlack, GColorBlack, GColorWhite };

GColor active_template_bg_color(void) {
    return s_template_bg_colors[s_active_template];
}

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

    // Divider below the header slot(s) and above the stat rows — skipped
    // entirely for an all-card template (compute_separator_top()'s "header
    // region above a stats region" assumption doesn't apply, and a floating
    // gray line would look out of place next to bordered cards). General
    // predicate, not a hardcoded template index, so any future all-card
    // template gets this for free too.
    bool needs_separator = false;
    for (int i = 0; i < s_slot_count; i++) {
        if (specs[i].size_class != SLOT_SIZE_CARD) {
            needs_separator = true;
            break;
        }
    }
    if (needs_separator) {
        int separator_top = compute_separator_top(specs, s_slot_count);
        s_separator_layer = layer_create(GRect(10, separator_top, bounds.size.w - 20, 1));
        layer_set_update_proc(s_separator_layer, separator_update_proc);
        layer_add_child(window_layer, s_separator_layer);
    } else {
        s_separator_layer = NULL;
    }
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
