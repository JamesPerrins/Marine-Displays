#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

#include <Arduino.h>

// Start the MQTT client task.
// broker       : hostname or IP of the MQTT broker (e.g. "192.168.1.10")
// port         : broker port (default 1883)
// user         : username (empty string = no auth)
// pass         : password
// topic_prefix : prefix stripped from incoming topics before SK path matching
//                e.g. "vessels/self" → subscribes to "vessels/self/#"
//                Leave empty to subscribe to "#" (all topics, useful for diagnosis)
void enable_mqtt(const char* broker, uint16_t port,
                 const char* user, const char* pass,
                 const char* topic_prefix);

// Stop the MQTT client task and disconnect.
void disable_mqtt();

// Temporarily disconnect while the config UI is open to free Core 0 bandwidth.
// Safe to call when MQTT is not enabled (no-op).
void pause_mqtt();
// Allow MQTT to reconnect after config save.
void resume_mqtt();

// Returns true if the MQTT task is running.
bool is_mqtt_enabled();

// Returns true if currently connected to the broker.
bool is_mqtt_connected();

#endif // MQTT_CONFIG_H