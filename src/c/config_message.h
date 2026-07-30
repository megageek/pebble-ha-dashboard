#pragma once
#include "types.h"

// AppMessage inbox handling: loading persisted Clay config at startup, and
// applying every subsequent inbox message — layout changes (TEMPLATE_INDEX/
// SLOT_n_CHANNEL), HA channel pushes, per-measure reporting toggles, status
// dot config, and local channel colors. This is the code path most
// directly involved in the ongoing config-save AppMessage investigation
// (see CLAUDE.md/HA_INTEGRATION_SPEC.md) — kept in its own file so it can
// be reviewed independently of rendering/layout/reporting concerns.

// Loads template choice, per-slot channel overrides, reporting toggles,
// dot config, and local channel colors persisted by a previous Clay config
// save, falling back to defaults on first run or if a stored value is out
// of range. Called once from main.c's main_window_load().
void load_config(void);

void inbox_received_callback(DictionaryIterator *iterator, void *context);
void inbox_dropped_callback(AppMessageResult reason, void *context);
