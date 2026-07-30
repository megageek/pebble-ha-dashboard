#pragma once
#include "types.h"

// Slot/separator drawing. slot_update_proc() is the Layer update_proc
// attached to every slot layer by layout.c's load_active_layout(); it
// dispatches to the right draw style for the slot's size class, and
// (when the slot is the designated dot-group host) reserves a strip for
// status_dots.c's draw_dot_group() before drawing the channel's normal
// content in whatever space remains.

void slot_update_proc(Layer *layer, GContext *ctx);
void separator_update_proc(Layer *layer, GContext *ctx);
