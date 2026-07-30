#pragma once
#include "types.h"

// Keeps the local display channels (time, date, battery, steps, and the
// Health-derived ones) fresh. Same underlying data as health_report.c's
// outbound status report, but deliberately independent of the
// REPORT_ENABLE_* toggles — those only gate what's sent to HA, not what
// the watch itself can display.

void update_time(void);
void update_battery(void);
void update_steps(void);
void update_health_channels(void);

// Subscribed in main.c's init(): battery_state_service_subscribe() and
// tick_timer_service_subscribe(MINUTE_UNIT, ...) respectively.
void battery_callback(BatteryChargeState state);
void tick_handler(struct tm *tick_time, TimeUnits units_changed);
