#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdint.h>

// Maximum number of AIS targets to track and render
#define AIS_MAX_TARGETS 20

// AIS range presets (nautical miles) — stored in ScreenConfig.graph_time_range
// (repurposed for AIS when display_type == DISPLAY_TYPE_AIS)
typedef enum {
    AIS_RANGE_0_5NM = 0,  // 0.5 NM
    AIS_RANGE_1NM   = 1,  // 1 NM
    AIS_RANGE_2NM   = 2,  // 2 NM
    AIS_RANGE_5NM   = 3,  // 5 NM
    AIS_RANGE_10NM  = 4,  // 10 NM
    AIS_RANGE_20NM  = 5   // 20 NM
} AisRange;

// Create the AIS radar display on the given screen (screen_num 0-4).
void ais_display_create(int screen_num);

// Redraw the AIS display with current data.
// own_lat, own_lon: own ship position (degrees)
// own_cog: own course over ground (degrees true)
// own_sog: own speed over ground (knots)
void ais_display_update(int screen_num, float own_lat, float own_lon,
                        float own_cog, float own_sog);

// Remove all LVGL objects owned by the AIS display for the given screen.
void ais_display_destroy(int screen_num);

// Fetch AIS target data from Signal K REST API.
// Call periodically (every ~5 seconds) from a task or the main loop.
// server_ip and server_port are the Signal K server connection details.
void ais_fetch_targets(const char* server_ip, uint16_t server_port);

#ifdef __cplusplus
}
#endif
