#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

#include <Arduino.h>

// Start the MQTT client task.
// broker     : hostname or IP of the MQTT broker (e.g. "192.168.1.10")
// port       : broker port (default 1883)
// user       : username (empty string = no auth)
// pass       : password
// system_id  : signalk-mqtt-bridge systemId (used in topic prefix N/<systemId>/...)
void enable_mqtt(const char* broker, uint16_t port,
                 const char* user, const char* pass,
                 const char* system_id);

// Stop the MQTT client task and disconnect.
void disable_mqtt();

// Returns true if the MQTT task is running.
bool is_mqtt_enabled();

// Returns true if currently connected to the broker.
bool is_mqtt_connected();

#endif // MQTT_CONFIG_H