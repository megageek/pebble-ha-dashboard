#include "health_report.h"
#include <stdio.h>

static const char *const REPORT_GROUP_NAMES[NUM_REPORT_GROUPS] = {
    "battery", "steps", "activity", "sleep", "heart_rate", "connected", "device_info",
};

bool s_report_enabled[NUM_REPORT_GROUPS] = {
    true, true, true, true, true, true, true,
};

static void write_health_metric_if_available(DictionaryIterator *iter, uint32_t key,
                                              HealthMetric metric, time_t start, time_t end) {
    HealthServiceAccessibilityMask mask = health_service_metric_accessible(metric, start, end);
    if (mask & HealthServiceAccessibilityMaskAvailable) {
        HealthValue value = health_service_sum(metric, start, end);
        dict_write_int32(iter, key, (int32_t)value);
    }
}

// Comma-joins the names of every currently-disabled group so HA can tell
// "user turned this off" apart from "not accessible on this hardware" (the
// latter is just a field that's silently absent, same as it always was).
static void write_disabled_groups(DictionaryIterator *iter) {
    char buffer[80];
    buffer[0] = '\0';
    bool any = false;

    for (int i = 0; i < NUM_REPORT_GROUPS; i++) {
        if (s_report_enabled[i]) {
            continue;
        }
        if (any) {
            strncat(buffer, ",", sizeof(buffer) - strlen(buffer) - 1);
        }
        strncat(buffer, REPORT_GROUP_NAMES[i], sizeof(buffer) - strlen(buffer) - 1);
        any = true;
    }

    if (any) {
        dict_write_cstring(iter, MESSAGE_KEY_REPORT_DISABLED, buffer);
    }
}

void send_status_report(void) {
    DictionaryIterator *iter;
    AppMessageResult begin_result = app_message_outbox_begin(&iter);
    if (begin_result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "send_status_report: outbox_begin failed, result=%d", (int)begin_result);
        return;  // outbox busy; the next periodic call will try again
    }

    if (s_report_enabled[REPORT_GROUP_BATTERY]) {
        BatteryChargeState battery = battery_state_service_peek();
        dict_write_int32(iter, MESSAGE_KEY_REPORT_BATTERY_PERCENT, battery.charge_percent);
        dict_write_int32(iter, MESSAGE_KEY_REPORT_BATTERY_CHARGING, battery.is_charging ? 1 : 0);
    }

    if (s_report_enabled[REPORT_GROUP_CONNECTED]) {
        dict_write_int32(iter, MESSAGE_KEY_REPORT_CONNECTED,
                          connection_service_peek_pebble_app_connection() ? 1 : 0);
    }

    time_t day_start = time_start_of_today();
    time_t now = time(NULL);

    if (s_report_enabled[REPORT_GROUP_STEPS]) {
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_STEPS, HealthMetricStepCount, day_start, now);
    }

    if (s_report_enabled[REPORT_GROUP_ACTIVITY]) {
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_ACTIVE_SECONDS, HealthMetricActiveSeconds, day_start, now);
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_DISTANCE_METERS, HealthMetricWalkedDistanceMeters, day_start, now);
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_ACTIVE_KCAL, HealthMetricActiveKCalories, day_start, now);
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_RESTING_KCAL, HealthMetricRestingKCalories, day_start, now);
    }

    if (s_report_enabled[REPORT_GROUP_SLEEP]) {
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_SLEEP_SECONDS, HealthMetricSleepSeconds, day_start, now);
        write_health_metric_if_available(iter, MESSAGE_KEY_REPORT_SLEEP_RESTFUL_SECONDS, HealthMetricSleepRestfulSeconds, day_start, now);
    }

    // Heart rate is an instantaneous reading, not a daily sum — accessibility
    // is checked for "right now", and health_service_peek_current_value()
    // (not _sum_today()) returns the latest sample. This deliberately relies
    // on the system's own default background HR sampling rather than
    // requesting an elevated sample rate — whatever value is already cached
    // (per Pebble's docs, at most ~15 min old) is what gets reported, no
    // extra battery cost incurred by this watchface.
    if (s_report_enabled[REPORT_GROUP_HEART_RATE]) {
        HealthServiceAccessibilityMask hr_mask = health_service_metric_accessible(HealthMetricHeartRateBPM, now, now);
        if (hr_mask & HealthServiceAccessibilityMaskAvailable) {
            HealthValue hr = health_service_peek_current_value(HealthMetricHeartRateBPM);
            dict_write_int32(iter, MESSAGE_KEY_REPORT_HEART_RATE_BPM, (int32_t)hr);
        }
    }

    write_disabled_groups(iter);

    AppMessageResult send_result = app_message_outbox_send();
    APP_LOG(APP_LOG_LEVEL_DEBUG, "send_status_report: outbox_send result=%d", (int)send_result);
}

static const char *watch_model_name(void) {
    switch (watch_info_get_model()) {
        case WATCH_INFO_MODEL_PEBBLE_TIME_2: return "Pebble Time 2";
        case WATCH_INFO_MODEL_COREDEVICES_PT2: return "Pebble Time 2 (Core Devices)";
        default: return "Unknown";
    }
}

static const char *watch_color_name(void) {
    switch (watch_info_get_color()) {
        case WATCH_INFO_COLOR_PEBBLE_TIME_2_BLACK: return "Black";
        case WATCH_INFO_COLOR_PEBBLE_TIME_2_SILVER: return "Silver";
        case WATCH_INFO_COLOR_PEBBLE_TIME_2_GOLD: return "Gold";
        case WATCH_INFO_COLOR_COREDEVICES_PT2_BLACK_GREY: return "Black/Grey";
        case WATCH_INFO_COLOR_COREDEVICES_PT2_BLACK_RED: return "Black/Red";
        case WATCH_INFO_COLOR_COREDEVICES_PT2_SILVER_BLUE: return "Silver/Blue";
        case WATCH_INFO_COLOR_COREDEVICES_PT2_SILVER_GREY: return "Silver/Grey";
        default: return "Unknown";
    }
}

void send_device_info(void) {
    if (!s_report_enabled[REPORT_GROUP_DEVICE_INFO]) {
        return;  // its disabled state still surfaces via write_disabled_groups()
    }

    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
        return;
    }

    WatchInfoVersion fw = watch_info_get_firmware_version();
    char fw_buffer[16];
    snprintf(fw_buffer, sizeof(fw_buffer), "%d.%d.%d", fw.major, fw.minor, fw.patch);

    dict_write_cstring(iter, MESSAGE_KEY_REPORT_MODEL, watch_model_name());
    dict_write_cstring(iter, MESSAGE_KEY_REPORT_FIRMWARE, fw_buffer);
    dict_write_cstring(iter, MESSAGE_KEY_REPORT_COLOR, watch_color_name());

    app_message_outbox_send();
}

void deferred_status_report_callback(void *context) {
    send_status_report();
}

void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "outbox send failed: reason=%d", (int)reason);
}

void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "outbox send acked");
}
