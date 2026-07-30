#pragma once
#include "types.h"

// Outbound reporting: watch -> phone -> Home Assistant.
//
// The watch reports its own status back out via AppMessage; pkjs relays it
// to HA over the same WebSocket connection used to receive channel data
// (see HA_INTEGRATION_SPEC.md). Every metric here is read from a service the
// watchface already touches for its own display (battery, steps) or is a
// cheap additional read — nothing here adds a new wake-up source or extra
// battery cost. Heart rate deliberately reads whatever the system's own
// default background sampling already produced
// (health_service_peek_current_value()) rather than requesting an elevated
// sample rate, so it's exactly as cheap as everything else here.
//
// The periodic report (as opposed to the immediate one on a battery
// charge-state change) is only attempted every STATUS_REPORT_INTERVAL_MINUTES
// wall-clock minutes, not every MINUTE_UNIT tick — see tick_handler() in
// channel_updates.c. Steps/activity/heart rate drift almost every minute
// while the watch is worn, which defeated send_status_report()'s own
// change-detection dedup below when checked that often; 5 minutes is a
// cadence HA doesn't need finer than, per the user.
//
// Each measure belongs to a ReportGroup the user can disable via Clay
// config; disabling one doesn't just stop sending its fields — every
// report also carries a REPORT_DISABLED list of currently-disabled group
// names, so the HA-side integration can tell "never available on this
// hardware" (field simply absent) apart from "user turned this off"
// (group named in REPORT_DISABLED) and remove/deactivate the right
// entities accordingly.

#define STATUS_REPORT_INTERVAL_MINUTES 5

// All enabled by default; persisted overrides loaded by
// config_message.c's load_config().
extern bool s_report_enabled[NUM_REPORT_GROUPS];

void send_status_report(void);

// Static device info, sent once at startup rather than on every tick.
void send_device_info(void);

// Registered in main.c's init(); also used by config_message.c to report
// promptly on a reporting-toggle change instead of waiting for the next tick.
void deferred_status_report_callback(void *context);

void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context);
void outbox_sent_callback(DictionaryIterator *iterator, void *context);
