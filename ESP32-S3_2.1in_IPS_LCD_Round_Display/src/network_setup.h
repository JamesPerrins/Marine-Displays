
#ifndef NETWORK_SETUP_H
#define NETWORK_SETUP_H

#include <Arduino.h>
#include <WebServer.h>
#include "signalk_config.h"  // For NUM_SCREENS, TOTAL_PARAMS

#include "calibration_types.h"
extern GaugeCalibrationPoint gauge_cal[NUM_SCREENS][2][5];

// Global web server instance
extern WebServer config_server;

// Auto-scroll interval in seconds (0 = off)
extern uint16_t auto_scroll_sec;

// Request the UI to change auto-scroll interval at runtime
void set_auto_scroll_interval(uint16_t sec);

// Initialize network (WiFi + WebServer) with web UI for configuration
void setup_network();

// Check if WiFi is connected
bool is_wifi_connected();

// Get configured Signal K server IP (empty string if not configured)
String get_signalk_server_ip();

// Get configured Signal K port
uint16_t get_signalk_server_port();

// Get Cloudflare Access credentials (empty strings if not configured)
String get_cf_client_id();
String get_cf_client_secret();

// Get/set configured Signal K path by index (0-9)
String get_signalk_path_by_index(int index);
void set_signalk_path_by_index(int index, const String& path);

// Data source and MQTT configuration getters
String   get_data_source();
String   get_mqtt_broker();
uint16_t get_mqtt_port();
String   get_mqtt_user();
String   get_mqtt_pass();
String   get_mqtt_topic_prefix();

// Load persisted preferences and screen configs (from NVS or SD fallback)
void load_preferences();

// Dump loaded `screen_configs` to the log for debugging
void dump_screen_configs();

// Backward compatibility helpers
inline String get_signalk_path1() { return get_signalk_path_by_index(0); }
inline String get_signalk_path2() { return get_signalk_path_by_index(1); }

#endif // SENSEXP_SETUP_H
