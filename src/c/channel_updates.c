#include "channel_updates.h"
#include "channels.h"
#include "layout.h"
#include "health_report.h"
#include <string.h>

void update_time(void) {
    time_t temp = time(NULL);
    struct tm *tick_time = localtime(&temp);
    if (!tick_time) {
        return;
    }

    strftime(s_channels[CHANNEL_TIME].text, sizeof(s_channels[CHANNEL_TIME].text),
              clock_is_24h_style() ? "%H:%M" : "%I:%M", tick_time);
    strftime(s_channels[CHANNEL_DATE].text, sizeof(s_channels[CHANNEL_DATE].text),
              "%a %d %b", tick_time);
    strftime(s_channels[CHANNEL_DAY_NAME].text, sizeof(s_channels[CHANNEL_DAY_NAME].text),
              "%a", tick_time);
    strftime(s_channels[CHANNEL_DATE_SHORT].text, sizeof(s_channels[CHANNEL_DATE_SHORT].text),
              "%d %b", tick_time);
    strftime(s_channels[CHANNEL_YEAR].text, sizeof(s_channels[CHANNEL_YEAR].text),
              "%Y", tick_time);

    mark_all_slots_dirty();
}

void update_battery(void) {
    BatteryChargeState state = battery_state_service_peek();
    s_channels[CHANNEL_BATTERY].value = state.charge_percent;
    s_channels[CHANNEL_BATTERY_CHARGING].value = state.is_charging ? 1 : 0;
    mark_all_slots_dirty();
}

// Shared by CHANNEL_STEPS and the other health-metric channels below: sets
// kind=TEXT "N/A" when the metric isn't accessible (no permission,
// unsupported hardware, or no data for the range), otherwise kind=NUMERIC
// with the (optionally scaled, e.g. seconds->minutes) summed value.
static void update_numeric_health_channel(ChannelIndex channel, HealthMetric metric,
                                           time_t start, time_t end, int32_t divisor) {
    ChannelData *ch = &s_channels[channel];
    HealthServiceAccessibilityMask mask = health_service_metric_accessible(metric, start, end);
    if (mask & HealthServiceAccessibilityMaskAvailable) {
        ch->kind = CHANNEL_KIND_NUMERIC;
        HealthValue value = health_service_sum(metric, start, end);
        ch->value = divisor > 1 ? (int32_t)value / divisor : (int32_t)value;
    } else {
        ch->kind = CHANNEL_KIND_TEXT;
        strncpy(ch->text, "N/A", sizeof(ch->text) - 1);
        ch->text[sizeof(ch->text) - 1] = '\0';
    }
}

void update_steps(void) {
    update_numeric_health_channel(CHANNEL_STEPS, HealthMetricStepCount, time_start_of_today(), time(NULL), 1);
    mark_all_slots_dirty();
}

void update_health_channels(void) {
    time_t day_start = time_start_of_today();
    time_t now = time(NULL);

    update_numeric_health_channel(CHANNEL_ACTIVE_MINUTES, HealthMetricActiveSeconds, day_start, now, 60);
    update_numeric_health_channel(CHANNEL_DISTANCE, HealthMetricWalkedDistanceMeters, day_start, now, 1);
    update_numeric_health_channel(CHANNEL_ACTIVE_KCAL, HealthMetricActiveKCalories, day_start, now, 1);
    update_numeric_health_channel(CHANNEL_RESTING_KCAL, HealthMetricRestingKCalories, day_start, now, 1);
    update_numeric_health_channel(CHANNEL_SLEEP_MINUTES, HealthMetricSleepSeconds, day_start, now, 60);
    update_numeric_health_channel(CHANNEL_SLEEP_RESTFUL_MINUTES, HealthMetricSleepRestfulSeconds, day_start, now, 60);

    // Heart rate is instantaneous, not summed — see health_report.c's
    // send_status_report() for why this deliberately reads the default
    // background sample rather than requesting a fresher one.
    ChannelData *hr = &s_channels[CHANNEL_HEART_RATE];
    HealthServiceAccessibilityMask hr_mask = health_service_metric_accessible(HealthMetricHeartRateBPM, now, now);
    if (hr_mask & HealthServiceAccessibilityMaskAvailable) {
        hr->kind = CHANNEL_KIND_NUMERIC;
        hr->value = (int32_t)health_service_peek_current_value(HealthMetricHeartRateBPM);
    } else {
        hr->kind = CHANNEL_KIND_TEXT;
        strncpy(hr->text, "N/A", sizeof(hr->text) - 1);
        hr->text[sizeof(hr->text) - 1] = '\0';
    }

    s_channels[CHANNEL_CONNECTED].value = connection_service_peek_pebble_app_connection() ? 1 : 0;

    mark_all_slots_dirty();
}

void battery_callback(BatteryChargeState state) {
    s_channels[CHANNEL_BATTERY].value = state.charge_percent;
    s_channels[CHANNEL_BATTERY_CHARGING].value = state.is_charging ? 1 : 0;
    mark_all_slots_dirty();
    send_status_report();
}

void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    update_time();
    update_steps();
    update_health_channels();

    // The on-watch channels above still refresh every minute (cheap, and
    // the whole point of a watch face), but the HA-bound status report is
    // only attempted on 5-minute wall-clock boundaries — steps/heart
    // rate/activity change almost every single minute while the watch is
    // actively worn, which defeated send_status_report()'s own
    // change-detection dedup (see health_report.c) and meant a real
    // AppMessage/BLE wake every minute anyway. Battery charge-state
    // changes still report immediately via battery_callback(), since
    // those are discrete events worth surfacing promptly rather than
    // values that drift continuously.
    if (tick_time->tm_min % STATUS_REPORT_INTERVAL_MINUTES == 0) {
        send_status_report();
    }
}
