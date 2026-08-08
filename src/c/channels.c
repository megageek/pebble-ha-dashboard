#include "channels.h"
#include <string.h>

ChannelData s_channels[NUM_CHANNELS];

const ChannelIndex local_color_channels[NUM_LOCAL_COLOR_CHANNELS] = {
    CHANNEL_TIME, CHANNEL_DATE, CHANNEL_BATTERY, CHANNEL_STEPS,
    CHANNEL_BATTERY_CHARGING, CHANNEL_CONNECTED, CHANNEL_ACTIVE_MINUTES, CHANNEL_DISTANCE,
    CHANNEL_ACTIVE_KCAL, CHANNEL_RESTING_KCAL, CHANNEL_SLEEP_MINUTES, CHANNEL_SLEEP_RESTFUL_MINUTES,
    CHANNEL_HEART_RATE, CHANNEL_HA_CONNECTED,
};

void init_channels(void) {
    s_channels[CHANNEL_TIME] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "TIME",
    };
    s_channels[CHANNEL_DATE] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "DATE",
    };
    s_channels[CHANNEL_BATTERY] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_BAR, .label = "BATT",
        .min = 0, .max = 100, .has_range = true, .unit = "%",
    };
    s_channels[CHANNEL_STEPS] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "STEPS",
    };
    // Remote channels pushed by Home Assistant via the pkjs WebSocket bridge.
    // Text until first update arrives; HA can repurpose a channel's kind
    // implicitly (values always arrive as strings over AppMessage today).
    // Default dot colors before HA ever sends its own — HA can override
    // on_color/off_color/hide_when per channel at any time (see
    // config_message.c).
    s_channels[CHANNEL_HA_1] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA1", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_2] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA2", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_3] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA3", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_4] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA4", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_5] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA5", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_6] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA6", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_7] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA7", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_8] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA8", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_9] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA9", .text = "--",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_HA_10] = (ChannelData) {
        .kind = CHANNEL_KIND_TEXT, .style = CHANNEL_STYLE_RAW, .label = "HA10", .text = "--",
        .on_color = "green", .off_color = "red",
    };

    // Local health/status channels — same source data as the outbound
    // status report, but selectable into any slot regardless of whether
    // reporting to HA is enabled for that measure. Populated by
    // channel_updates.c/health_report.c; kind may flip to TEXT ("N/A") if a
    // metric isn't accessible. Dot colors here are defaults; overridable
    // via Clay (see config_message.c's load_config()).
    s_channels[CHANNEL_BATTERY_CHARGING] = (ChannelData) {
        .kind = CHANNEL_KIND_BINARY, .style = CHANNEL_STYLE_RAW, .label = "CHARGE",
        .on_color = "green", .off_color = "gray",
    };
    s_channels[CHANNEL_CONNECTED] = (ChannelData) {
        .kind = CHANNEL_KIND_BINARY, .style = CHANNEL_STYLE_RAW, .label = "CONN",
        .on_color = "green", .off_color = "red",
    };
    s_channels[CHANNEL_ACTIVE_MINUTES] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "ACTIVE", .unit = "min",
    };
    s_channels[CHANNEL_DISTANCE] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "DIST", .unit = "m",
    };
    s_channels[CHANNEL_ACTIVE_KCAL] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "ACAL", .unit = "kcal",
    };
    s_channels[CHANNEL_RESTING_KCAL] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "RCAL", .unit = "kcal",
    };
    s_channels[CHANNEL_SLEEP_MINUTES] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "SLEEP", .unit = "min",
    };
    s_channels[CHANNEL_SLEEP_RESTFUL_MINUTES] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "RSLEEP", .unit = "min",
    };
    s_channels[CHANNEL_HEART_RATE] = (ChannelData) {
        .kind = CHANNEL_KIND_NUMERIC, .style = CHANNEL_STYLE_RAW, .label = "HR", .unit = "bpm",
    };

    // The phone's live connection state to Home Assistant — pushed by pkjs
    // via HA_CONN_VALUE (see inbox_received_callback() in
    // config_message.c), not computed from any on-watch API. .value left
    // unset (zero-init) means "disconnected" until pkjs's first push, same
    // as every other binary channel here.
    s_channels[CHANNEL_HA_CONNECTED] = (ChannelData) {
        .kind = CHANNEL_KIND_BINARY, .style = CHANNEL_STYLE_RAW, .label = "HA CONN",
        .on_color = "green", .off_color = "red",
    };
}

GColor color_from_name(const char *name) {
    if (strcmp(name, "red") == 0) return GColorRed;
    if (strcmp(name, "orange") == 0) return GColorOrange;
    if (strcmp(name, "yellow") == 0) return GColorYellow;
    if (strcmp(name, "green") == 0) return GColorGreen;
    if (strcmp(name, "blue") == 0) return GColorBlue;
    if (strcmp(name, "purple") == 0) return GColorPurple;
    if (strcmp(name, "white") == 0) return GColorWhite;
    if (strcmp(name, "gray") == 0) return GColorLightGray;
    return GColorLightGray;
}

int8_t parse_hide_when(const char *s) {
    if (strcmp(s, "on") == 0) return 1;
    if (strcmp(s, "off") == 0) return 2;
    return 0;
}

bool parse_channel_kind(const char *s, ChannelKind *out) {
    if (strcmp(s, "text") == 0) { *out = CHANNEL_KIND_TEXT; return true; }
    if (strcmp(s, "numeric") == 0) { *out = CHANNEL_KIND_NUMERIC; return true; }
    if (strcmp(s, "binary") == 0) { *out = CHANNEL_KIND_BINARY; return true; }
    return false;
}

bool parse_channel_style(const char *s, ChannelStyle *out) {
    if (strcmp(s, "raw") == 0) { *out = CHANNEL_STYLE_RAW; return true; }
    if (strcmp(s, "bar") == 0) { *out = CHANNEL_STYLE_BAR; return true; }
    return false;
}

bool channel_is_on(ChannelData *ch) {
    switch (ch->kind) {
        case CHANNEL_KIND_BINARY:
        case CHANNEL_KIND_NUMERIC:
            return ch->value != 0;
        case CHANNEL_KIND_TEXT:
        default:
            return strcmp(ch->text, "on") == 0;
    }
}
