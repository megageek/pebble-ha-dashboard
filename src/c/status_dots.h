#pragma once
#include "types.h"

// Status dot group: up to MAX_DOTS channels rendered as small colored
// circles, for binary-ish measures that don't warrant a full slot of their
// own. One shared group per layout (not per-slot), attached to whichever
// configurable slot index s_dot_group_slot names — additively, alongside
// that slot's normal channel content, not replacing it. -1 = disabled /
// no channel assigned to that dot position.

extern int s_dot_group_slot;  // which of the 10 configurable slots also shows dots; -1 = none
extern int s_dot_channels[MAX_DOTS];

extern const int DOT_DIAMETERS[NUM_DOT_SIZES];
extern const int DOT_SPACINGS[NUM_DOT_SIZES];
extern int s_dot_size;

int count_assigned_dots(void);

// Draws centered within whatever (sub-)rect it's given — the caller
// (rendering.c's slot_update_proc) carves out a reserved strip on the
// right of the slot for this, rather than this function claiming the
// whole slot.
void draw_dot_group(GContext *ctx, GRect bounds);

// Shared by the two local dot-capable channels (battery charging, phone
// connection) to apply+persist on_color/off_color/hide_when tuples from a
// Clay save. Returns true if anything actually changed, so the caller can
// fold that into its own "does this need a redraw" tracking.
bool apply_dot_color_config(DictionaryIterator *iterator, ChannelIndex channel,
                             uint32_t on_color_key, uint32_t off_color_key, uint32_t hide_when_key,
                             int persist_on_color_key, int persist_off_color_key,
                             int persist_hide_when_key);
