#pragma once
#include "types.h"

// Layout templates and the slot-layer lifecycle built from them. A
// template is a function returning, given the window bounds, an array of
// SlotSpec — a default channel assignment per screen position. The active
// template (and any per-slot channel overrides from Clay config) determine
// what load_active_layout() actually builds each time the layout changes.

#define MAX_SLOTS 10

typedef int (*TemplateBuilder)(GRect bounds, SlotSpec *out);

// Number of built-in templates (currently 2) — needed by config_message.c
// to validate an incoming TEMPLATE_INDEX before accepting it.
extern const int NUM_TEMPLATES;

// The single shared app window — owned by main.c (created/destroyed in
// init()/deinit(), handlers set in main_window_load()/unload()), but
// needed here (and by config_message.c, to get its root layer before a
// layout rebuild) since layout teardown/rebuild always targets it.
extern Window *s_main_window;

extern int s_active_template;
// -1 means "use the template's default channel for this slot".
extern int s_slot_channel_override[NUM_CONFIGURABLE_SLOTS];

// Builds slot layers for the active template + config overrides. Safe to
// call again after destroy_active_layout() when config changes.
void load_active_layout(Layer *window_layer, GRect bounds);
void destroy_active_layout(void);

void mark_all_slots_dirty(void);
