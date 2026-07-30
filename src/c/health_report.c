#include "health_report.h"
#include <stdio.h>

static const char *const REPORT_GROUP_NAMES[NUM_REPORT_GROUPS] = {
    "battery", "steps", "activity", "sleep", "heart_rate", "device_info",
};

bool s_report_enabled[NUM_REPORT_GROUPS] = {
    true, true, true, true, true, true,
};

// Every field this report can carry, plus whether each optional one is
// actually present this time (a metric can flip between accessible/
// inaccessible, and that transition itself is a real change worth
// sending). Compared field-by-field against the previous send below so an
// unchanged report costs nothing — HA keeps an entity's last value until a
// new one arrives, so re-sending identical data every single minute, all
// day, wakes the Bluetooth radio for no informational gain.
typedef struct {
    bool populated;  // false only before the very first report
    bool battery_present;
    int32_t battery_percent;
    bool battery_charging;
    bool steps_present;
    int32_t steps;
    bool activity_present;
    int32_t active_seconds;
    int32_t distance_meters;
    int32_t active_kcal;
    int32_t resting_kcal;
    bool sleep_present;
    int32_t sleep_seconds;
    int32_t sleep_restful_seconds;
    bool heart_rate_present;
    int32_t heart_rate_bpm;
    char disabled[80];
} StatusReportSnapshot;

static StatusReportSnapshot s_last_report;

static bool snapshot_equal(const StatusReportSnapshot *a, const StatusReportSnapshot *b) {
    return a->battery_present == b->battery_present
        && (!a->battery_present || (a->battery_percent == b->battery_percent && a->battery_charging == b->battery_charging))
        && a->steps_present == b->steps_present
        && (!a->steps_present || a->steps == b->steps)
        && a->activity_present == b->activity_present
        && (!a->activity_present || (a->active_seconds == b->active_seconds && a->distance_meters == b->distance_meters
                                      && a->active_kcal == b->active_kcal && a->resting_kcal == b->resting_kcal))
        && a->sleep_present == b->sleep_present
        && (!a->sleep_present || (a->sleep_seconds == b->sleep_seconds && a->sleep_restful_seconds == b->sleep_restful_seconds))
        && a->heart_rate_present == b->heart_rate_present
        && (!a->heart_rate_present || a->heart_rate_bpm == b->heart_rate_bpm)
        && strcmp(a->disabled, b->disabled) == 0;
}

void send_status_report(void) {
    StatusReportSnapshot snap = { .populated = true };

    if (s_report_enabled[REPORT_GROUP_BATTERY]) {
        BatteryChargeState battery = battery_state_service_peek();
        snap.battery_present = true;
        snap.battery_percent = battery.charge_percent;
        snap.battery_charging = battery.is_charging;
    }

    time_t day_start = time_start_of_today();
    time_t now = time(NULL);

    if (s_report_enabled[REPORT_GROUP_STEPS]) {
        HealthServiceAccessibilityMask mask = health_service_metric_accessible(HealthMetricStepCount, day_start, now);
        if (mask & HealthServiceAccessibilityMaskAvailable) {
            snap.steps_present = true;
            snap.steps = (int32_t)health_service_sum(HealthMetricStepCount, day_start, now);
        }
    }

    if (s_report_enabled[REPORT_GROUP_ACTIVITY]) {
        HealthServiceAccessibilityMask mask = health_service_metric_accessible(HealthMetricActiveSeconds, day_start, now);
        if (mask & HealthServiceAccessibilityMaskAvailable) {
            snap.activity_present = true;
            snap.active_seconds = (int32_t)health_service_sum(HealthMetricActiveSeconds, day_start, now);
            snap.distance_meters = (int32_t)health_service_sum(HealthMetricWalkedDistanceMeters, day_start, now);
            snap.active_kcal = (int32_t)health_service_sum(HealthMetricActiveKCalories, day_start, now);
            snap.resting_kcal = (int32_t)health_service_sum(HealthMetricRestingKCalories, day_start, now);
        }
    }

    if (s_report_enabled[REPORT_GROUP_SLEEP]) {
        HealthServiceAccessibilityMask mask = health_service_metric_accessible(HealthMetricSleepSeconds, day_start, now);
        if (mask & HealthServiceAccessibilityMaskAvailable) {
            snap.sleep_present = true;
            snap.sleep_seconds = (int32_t)health_service_sum(HealthMetricSleepSeconds, day_start, now);
            snap.sleep_restful_seconds = (int32_t)health_service_sum(HealthMetricSleepRestfulSeconds, day_start, now);
        }
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
            snap.heart_rate_present = true;
            snap.heart_rate_bpm = (int32_t)health_service_peek_current_value(HealthMetricHeartRateBPM);
        }
    }

    snap.disabled[0] = '\0';
    for (int i = 0; i < NUM_REPORT_GROUPS; i++) {
        if (s_report_enabled[i]) {
            continue;
        }
        if (snap.disabled[0] != '\0') {
            strncat(snap.disabled, ",", sizeof(snap.disabled) - strlen(snap.disabled) - 1);
        }
        strncat(snap.disabled, REPORT_GROUP_NAMES[i], sizeof(snap.disabled) - strlen(snap.disabled) - 1);
    }

    if (s_last_report.populated && snapshot_equal(&snap, &s_last_report)) {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "send_status_report: unchanged, skipping send");
        return;
    }

    DictionaryIterator *iter;
    AppMessageResult begin_result = app_message_outbox_begin(&iter);
    if (begin_result != APP_MSG_OK) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "send_status_report: outbox_begin failed, result=%d", (int)begin_result);
        return;  // outbox busy; the next periodic call will try again
    }

    if (snap.battery_present) {
        dict_write_int32(iter, MESSAGE_KEY_REPORT_BATTERY_PERCENT, snap.battery_percent);
        dict_write_int32(iter, MESSAGE_KEY_REPORT_BATTERY_CHARGING, snap.battery_charging ? 1 : 0);
    }
    if (snap.steps_present) {
        dict_write_int32(iter, MESSAGE_KEY_REPORT_STEPS, snap.steps);
    }
    if (snap.activity_present) {
        dict_write_int32(iter, MESSAGE_KEY_REPORT_ACTIVE_SECONDS, snap.active_seconds);
        dict_write_int32(iter, MESSAGE_KEY_REPORT_DISTANCE_METERS, snap.distance_meters);
        dict_write_int32(iter, MESSAGE_KEY_REPORT_ACTIVE_KCAL, snap.active_kcal);
        dict_write_int32(iter, MESSAGE_KEY_REPORT_RESTING_KCAL, snap.resting_kcal);
    }
    if (snap.sleep_present) {
        dict_write_int32(iter, MESSAGE_KEY_REPORT_SLEEP_SECONDS, snap.sleep_seconds);
        dict_write_int32(iter, MESSAGE_KEY_REPORT_SLEEP_RESTFUL_SECONDS, snap.sleep_restful_seconds);
    }
    if (snap.heart_rate_present) {
        dict_write_int32(iter, MESSAGE_KEY_REPORT_HEART_RATE_BPM, snap.heart_rate_bpm);
    }
    if (snap.disabled[0] != '\0') {
        dict_write_cstring(iter, MESSAGE_KEY_REPORT_DISABLED, snap.disabled);
    }

    AppMessageResult send_result = app_message_outbox_send();
    APP_LOG(APP_LOG_LEVEL_DEBUG, "send_status_report: outbox_send result=%d", (int)send_result);
    s_last_report = snap;
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
        return;  // its disabled state still surfaces via the `disabled` field send_status_report() builds
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
