#include "mqtt_config.h"
#include "signalk_config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ── Configuration ──────────────────────────────────────────────────────────
static String  s_broker    = "";
static uint16_t s_port     = 1883;
static String  s_user      = "";
static String  s_pass      = "";
static String  s_system_id = "";

// Topic prefix length cached after connect so the message callback can slice it cheaply
static int s_prefix_len = 0;   // length of "N/<systemId>/"
// Keepalive topic: R/<systemId>/keepalive
static String s_keepalive_topic = "";

// ── Runtime state ─────────────────────────────────────────────────────────
static WiFiClient     s_wifi_client;
static PubSubClient   s_mqtt_client(s_wifi_client);
static TaskHandle_t   s_mqtt_task_handle = NULL;
static volatile bool  s_mqtt_enabled     = false;
static volatile bool  s_mqtt_connected   = false;

// ── MQTT message callback ──────────────────────────────────────────────────
// Called by PubSubClient on the MQTT task when a subscribed message arrives.
// Topic format: N/<systemId>/<sk.path.with.dots>
// Payload     : plain ASCII float, e.g. "2.0665"
static void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    if (!topic || length == 0 || s_prefix_len <= 0) return;

    // Extract SignalK path from topic by stripping the prefix
    int topic_len = strlen(topic);
    if (topic_len <= s_prefix_len) return;
    const char* sk_path = topic + s_prefix_len;

    // Parse float from payload (payload is not null-terminated)
    char val_buf[32];
    unsigned int copy_len = (length < sizeof(val_buf) - 1) ? length : sizeof(val_buf) - 1;
    memcpy(val_buf, payload, copy_len);
    val_buf[copy_len] = '\0';
    float value = atof(val_buf);

    update_signalk_value(sk_path, value);
}

// ── Connect / subscribe helper ─────────────────────────────────────────────
static bool mqtt_connect() {
    if (s_broker.length() == 0) return false;

    // Build a unique client ID
    String client_id = "ESP32-Marine-";
    client_id += String((uint32_t)ESP.getEfuseMac(), HEX);

    bool ok;
    if (s_user.length() > 0) {
        ok = s_mqtt_client.connect(client_id.c_str(), s_user.c_str(), s_pass.c_str());
    } else {
        ok = s_mqtt_client.connect(client_id.c_str());
    }

    if (!ok) {
        Serial.printf("[MQTT] Connect failed, state=%d\n", s_mqtt_client.state());
        return false;
    }

    Serial.printf("[MQTT] Connected to %s:%u as %s\n",
                  s_broker.c_str(), s_port, client_id.c_str());

    // Subscribe to all paths under our systemId
    // Topic: N/<systemId>/#
    String sub_topic = "N/" + s_system_id + "/#";
    s_mqtt_client.subscribe(sub_topic.c_str());
    Serial.printf("[MQTT] Subscribed to %s\n", sub_topic.c_str());

    // Cache prefix length once connected
    // prefix = "N/<systemId>/"
    s_prefix_len = 3 + s_system_id.length();  // "N/" + systemId + "/"
    s_keepalive_topic = "R/" + s_system_id + "/keepalive";

    s_mqtt_connected = true;
    return true;
}

// ── FreeRTOS task ─────────────────────────────────────────────────────────
static void mqtt_task(void* param) {
    static const unsigned long RECONNECT_INTERVAL_MS = 5000;
    static const unsigned long KEEPALIVE_INTERVAL_MS = 30000;

    unsigned long last_reconnect_attempt = 0;
    unsigned long last_keepalive         = 0;

    s_mqtt_client.setServer(s_broker.c_str(), s_port);
    s_mqtt_client.setCallback(mqtt_callback);
    // Buffer large enough for a typical SK message (topic + payload)
    s_mqtt_client.setBufferSize(512);

    while (s_mqtt_enabled) {
        if (!WiFi.isConnected()) {
            s_mqtt_connected = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!s_mqtt_client.connected()) {
            s_mqtt_connected = false;
            unsigned long now = millis();
            if (now - last_reconnect_attempt >= RECONNECT_INTERVAL_MS) {
                last_reconnect_attempt = now;
                mqtt_connect();
            }
        } else {
            // Send keepalive so the signalk-mqtt-bridge doesn't time out (TTL ~60 s)
            unsigned long now = millis();
            if (now - last_keepalive >= KEEPALIVE_INTERVAL_MS) {
                last_keepalive = now;
                s_mqtt_client.publish(s_keepalive_topic.c_str(), "");
            }
        }

        s_mqtt_client.loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Clean up
    if (s_mqtt_client.connected()) {
        s_mqtt_client.disconnect();
    }
    s_mqtt_connected = false;
    s_mqtt_task_handle = NULL;
    vTaskDelete(NULL);
}

// ── Public API ─────────────────────────────────────────────────────────────
void enable_mqtt(const char* broker, uint16_t port,
                 const char* user, const char* pass,
                 const char* system_id) {
    if (s_mqtt_enabled) disable_mqtt();

    s_broker    = broker    ? broker    : "";
    s_port      = port ? port : 1883;
    s_user      = user      ? user      : "";
    s_pass      = pass      ? pass      : "";
    s_system_id = system_id ? system_id : "signalk";

    if (s_broker.length() == 0) {
        Serial.println("[MQTT] No broker configured, not starting");
        return;
    }

    s_mqtt_enabled   = true;
    s_mqtt_connected = false;

    Serial.printf("[MQTT] Starting task — broker=%s port=%u systemId=%s\n",
                  s_broker.c_str(), s_port, s_system_id.c_str());

    xTaskCreatePinnedToCore(
        mqtt_task,
        "mqtt_task",
        8192,       // stack — no SSL, so 8 KB is ample
        NULL,
        1,          // same priority as signalk_task
        &s_mqtt_task_handle,
        0           // Core 0, same as signalk_task (WiFi/network core)
    );
}

void disable_mqtt() {
    if (!s_mqtt_enabled) return;
    s_mqtt_enabled = false;
    // Give the task up to 2 s to exit cleanly
    for (int i = 0; i < 20 && s_mqtt_task_handle != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_mqtt_task_handle != NULL) {
        vTaskDelete(s_mqtt_task_handle);
        s_mqtt_task_handle = NULL;
    }
    s_mqtt_connected = false;
    Serial.println("[MQTT] Disabled");
}

bool is_mqtt_enabled()    { return s_mqtt_enabled; }
bool is_mqtt_connected()  { return s_mqtt_connected; }
