#include <pebble.h>
#include "types.h"
#include "channels.h"
#include "layout.h"
#include "channel_updates.h"
#include "health_report.h"
#include "config_message.h"

// App entry point: window lifecycle (main_window_load/unload) and
// init/deinit/main. Everything else — the channel data model, layout
// templates, drawing, status dots, health/status reporting, and AppMessage
// handling — lives in its own file; see types.h for the shared vocabulary
// and each module's own header for what it owns.

static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    window_set_background_color(window, GColorBlack);

    init_channels();
    load_config();
    load_active_layout(window_layer, bounds);

    update_time();
    update_battery();
    update_steps();
    update_health_channels();
}

static void main_window_unload(Window *window) {
    destroy_active_layout();
}

static void init(void) {
    s_main_window = window_create();
    window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load = main_window_load,
        .unload = main_window_unload,
    });
    window_stack_push(s_main_window, true);

    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
    battery_state_service_subscribe(battery_callback);

    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    app_message_register_outbox_sent(outbox_sent_callback);
    // Was app_message_inbox_size_maximum()/outbox_size_maximum() — the
    // platform maximum, 8200 bytes each (confirmed via APP_LOG at
    // app_message_open() time: "This consumes 8200 bytes of heap memory").
    // Real-device testing found every webviewclosed-triggered config send
    // failing outright regardless of content/size/timing/WebSocket
    // state/app identity — everything this repo's code controls. A
    // known-working reference watchface (peerdavid/pebble-mesh) requests
    // fixed, modest buffers (768/128) instead of the platform maximum.
    // 1024/512 here comfortably covers this app's largest actual
    // transactions (a 15-key config chunk, a 13-field HA channel update, a
    // ~12-field status report with the REPORT_DISABLED string) with margin,
    // while staying far below the maximum this app had been requesting.
    app_message_open(1024, 512);

    send_device_info();
    // The outbox is single-flight; allow the device-info transaction to
    // finish before producing the first status report.
    app_timer_register(1000, deferred_status_report_callback, NULL);
}

static void deinit(void) {
    tick_timer_service_unsubscribe();
    battery_state_service_unsubscribe();
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
    return 0;
}
