#pragma once
#include "types.h"

// The channel data model: the per-channel value/style/label state every
// slot renders from, plus the small parsing/lookup helpers that operate on
// it. Populated at startup by init_channels(), then mutated in place by
// config_message.c (Clay/HA pushes) and channel_updates.c (local
// time/battery/health refreshes) — this module owns the storage and the
// pure helper functions, not the code that writes into it.

extern ChannelData s_channels[NUM_CHANNELS];

// The 13 local (non-HA) channels that get Clay-configurable background/
// value/label colors — every channel except the 10 remote HA_1..HA_10
// ones, which instead take these fields over the wire from HA itself (see
// config_message.c). ChannelIndex values are compile-time enum constants,
// unlike MESSAGE_KEY_* elsewhere, so this can be a plain array shared by
// both config_message.c's load_config() and its color-apply logic.
extern const ChannelIndex local_color_channels[NUM_LOCAL_COLOR_CHANNELS];

void init_channels(void);

// Named palette shared with Clay (local channel color pickers) and with
// HA (on_color/off_color fields in pebble_channel_update) — see
// HA_INTEGRATION_SPEC.md. Unrecognized/empty names fall back to light gray
// rather than failing, since a color typo shouldn't crash rendering.
GColor color_from_name(const char *name);

// "none"/"on"/"off" -> 0/1/2. Same three-value vocabulary for Clay's local
// hide-when select and HA's hide_when field.
int8_t parse_hide_when(const char *s);

// "text"/"numeric"/"binary" -> ChannelKind, for HA's optional HAn_KIND
// field (see HA_INTEGRATION_SPEC.md). Unlike parse_hide_when()/
// parse_channel_style() below, an unrecognized string is left unhandled
// (returns false, *out untouched) rather than coerced to a default — an
// integration typo should leave the channel's last-known kind alone
// rather than silently flip it back to text.
bool parse_channel_kind(const char *s, ChannelKind *out);

// "raw"/"bar" -> ChannelStyle, for HA's optional HAn_STYLE field. Same
// unrecognized-is-ignored behavior as parse_channel_kind() above.
bool parse_channel_style(const char *s, ChannelStyle *out);

// A channel's "on" state for dot purposes, regardless of its kind: binary
// and numeric channels are on when non-zero; text channels (the only kind
// HA channels can be) are on when the value is exactly the lowercase
// string "on" — see HA_INTEGRATION_SPEC.md for why this is a fixed literal
// rather than a fuzzy/case-insensitive check.
bool channel_is_on(ChannelData *ch);
