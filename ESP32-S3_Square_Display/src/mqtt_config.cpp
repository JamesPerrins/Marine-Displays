#include "mqtt_config.h"
#include "signalk_config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>

// ── Configuration ──────────────────────────────────────────────────────────
static String   s_broker       = "";
static uint16_t s_port         = 1883;
static String   s_user         = "";
static String   s_pass         = "";
// Full topic prefix before the SK path, e.g. "N/signalk/23d7359e0d9e/vessels/self"
// Empty = subscribe to # (diagnostic mode, no prefix stripping).
static String   s_topic_prefix = "";
static int      s_prefix_len   = 0;   // length of prefix + trailing '/', cached after connect

// Keepalive topic and payload, derived from topic_prefix on connect.
// Bridge requires a publish to R/signalk/<systemId>/keepalive every <TTL (60s default)
// to stay active. Empty payload = send all deltas.
// Payload specifying paths:  '["vessels/self/#"]'
static String   s_keepalive_topic   = "";
static String   s_keepalive_payload = "";

// ── Runtime state ─────────────────────────────────────────────────────────
static WiFiClient    s_wifi_client;
static PubSubClient  s_mqtt_client(s_wifi_client);
static TaskHandle_t  s_mqtt_task_handle = NULL;
static volatile bool s_mqtt_enabled     = false;
static volatile bool s_mqtt_connected   = false;

// ── Extract systemId from topic prefix ────────────────────────────────────
// Prefix format: "N/signalk/<systemId>/vessels/self"
// Returns the <systemId> segment, e.g. "23d7359e0d9e"
static String extract_system_id(const String& prefix) {
    // Find the text between the 2nd and 3rd '/'
    // "N/signalk/<systemId>/..."
    //   0123456789
    int first_slash  = prefix.indexOf('/');           // after "N"
    if (first_slash < 0) return "";
    int second_slash = prefix.indexOf('/', first_slash + 1);  // after "signalk"
    if (second_slash < 0) return "";
    int third_slash  = prefix.indexOf('/', second_slash + 1); // after systemId
    if (third_slash < 0) {
        // prefix IS "N/signalk/<systemId>" with no trailing path
        return prefix.substring(second_slash + 1);
    }
    return prefix.substring(second_slash + 1, third_slash);
}

// Extract the path portion that follows the systemId in the prefix,
// e.g. "N/signalk/<systemId>/vessels/self" → "vessels/self"
// Used to build the keepalive payload scope.
static String extract_path_scope(const String& prefix) {
    int first_slash  = prefix.indexOf('/');
    if (first_slash < 0) return "";
    int second_slash = prefix.indexOf('/', first_slash + 1);
    if (second_slash < 0) return "";
    int third_slash  = prefix.indexOf('/', second_slash + 1);
    if (third_slash < 0) return "";  // no path after systemId
    return prefix.substring(third_slash + 1); // e.g. "vessels/self"
}

// ── MQTT message callback ──────────────────────────────────────────────────
static void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    if (!topic) return;

    // Diagnostic: log first 20 messages so the user can verify topic format
    static int diag_count = 0;
    if (diag_count < 20) {
        diag_count++;
        int preview = (length < 40) ? (int)length : 40;
        Serial.printf("[MQTT DIAG #%d] topic='%s' payload='%.*s'\n",
                      diag_count, topic, preview, (const char*)payload);
    }

    if (length == 0) return;  // keepalive acks etc. have empty payload

    // Strip prefix to get the SK path portion
    const char* path_start = topic;
    if (s_prefix_len > 0) {
        int topic_len = strlen(topic);
        if (topic_len <= s_prefix_len) return;
        path_start = topic + s_prefix_len;
    }

    // Convert path separators: '/' → '.' so "environment/wind/speedTrue"
    // matches the configured SK path "environment.wind.speedTrue"
    char sk_path[128];
    int out = 0;
    for (const char* p = path_start; *p && out < (int)sizeof(sk_path) - 1; p++) {
        sk_path[out++] = (*p == '/') ? '.' : *p;
    }
    sk_path[out] = '\0';

    // Parse numeric value from payload.
    // signalk-mqtt-bridge sends JSON: {"value":1.234,"$source":"..."}
    // Older/plain configs may send a bare float: "1.234"
    // We look for the "value": key; fall back to direct atof if not found.
    char val_buf[128];
    unsigned int copy_len = (length < sizeof(val_buf) - 1) ? length : sizeof(val_buf) - 1;
    memcpy(val_buf, payload, copy_len);
    val_buf[copy_len] = '\0';

    float value = 0.0f;
    const char* vp = strstr(val_buf, "\"value\"");
    if (vp) {
        const char* colon = strchr(vp + 7, ':');
        if (colon) value = atof(colon + 1);
    } else {
        value = atof(val_buf);  // plain float payload
    }

    update_signalk_value(sk_path, value);
}

// ── Connect, subscribe, and send initial keepalive ─────────────────────────
static bool mqtt_connect() {
    if (s_broker.length() == 0) return false;

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

    // Build subscription topic and prefix length
    String sub_topic;
    if (s_topic_prefix.length() > 0) {
        sub_topic    = s_topic_prefix + "/#";
        s_prefix_len = s_topic_prefix.length() + 1;  // +1 for trailing '/'
    } else {
        sub_topic    = "#";
        s_prefix_len = 0;
    }
    s_mqtt_client.subscribe(sub_topic.c_str());
    Serial.printf("[MQTT] Subscribed to '%s' (prefix_len=%d)\n",
                  sub_topic.c_str(), s_prefix_len);

    // Build keepalive topic and payload from prefix
    // Topic : R/signalk/<systemId>/keepalive
    // Payload: ["<pathScope>/#"] e.g. '["vessels/self/#"]'
    //          or "" to request all deltas
    String system_id = extract_system_id(s_topic_prefix);
    String scope     = extract_path_scope(s_topic_prefix);
    if (system_id.length() > 0) {
        s_keepalive_topic = "R/signalk/" + system_id + "/keepalive";
        if (scope.length() > 0) {
            s_keepalive_payload = "[\"" + scope + "/#\"]";
        } else {
            s_keepalive_payload = "";  // empty = all deltas
        }
        // Send immediately so bridge starts publishing right away
        s_mqtt_client.publish(s_keepalive_topic.c_str(), s_keepalive_payload.c_str());
        Serial.printf("[MQTT] Keepalive → topic='%s' payload='%s'\n",
                      s_keepalive_topic.c_str(), s_keepalive_payload.c_str());
    } else {
        // No systemId extractable (e.g. prefix is empty or plain)
        s_keepalive_topic = "";
        Serial.println("[MQTT] No systemId in prefix — keepalive disabled");
    }

    s_mqtt_connected = true;
    return true;
}

// ── FreeRTOS task ─────────────────────────────────────────────────────────
static void mqtt_task(void* param) {
    static const unsigned long RECONNECT_INTERVAL_MS = 5000;
    static const unsigned long KEEPALIVE_INTERVAL_MS = 30000;  // well within 60s TTL

    unsigned long last_reconnect_attempt = 0;
    unsigned long last_keepalive         = 0;

    s_mqtt_client.setServer(s_broker.c_str(), s_port);
    s_mqtt_client.setCallback(mqtt_callback);
    // JSON payloads from signalk-mqtt-bridge are ~80-120 bytes + topic overhead.
    // 512 bytes is sufficient; increase if very long path names are used.
    s_mqtt_client.setBufferSize(512);

    while (s_mqtt_enabled) {
        esp_task_wdt_reset();  // keep WDT happy on Core 0

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
                if (mqtt_connect()) {
                    last_keepalive = millis();
                }
            }
        } else {
            // Periodic keepalive to keep the bridge active
            unsigned long now = millis();
            if (s_keepalive_topic.length() > 0 &&
                now - last_keepalive >= KEEPALIVE_INTERVAL_MS) {
                last_keepalive = now;
                s_mqtt_client.publish(s_keepalive_topic.c_str(),
                                      s_keepalive_payload.c_str());
            }
        }

        s_mqtt_client.loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_mqtt_client.connected()) {
        s_mqtt_client.disconnect();
    }
    s_mqtt_connected   = false;
    s_mqtt_task_handle = NULL;
    vTaskDelete(NULL);
}

// ── Public API ─────────────────────────────────────────────────────────────
void enable_mqtt(const char* broker, uint16_t port,
                 const char* user,   const char* pass,
                 const char* topic_prefix) {
    if (s_mqtt_enabled) disable_mqtt();

    s_broker       = broker       ? broker       : "";
    s_port         = port ? port : 1883;
    s_user         = user         ? user         : "";
    s_pass         = pass         ? pass         : "";
    s_topic_prefix = topic_prefix ? topic_prefix : "";

    if (s_broker.length() == 0) {
        Serial.println("[MQTT] No broker configured, not starting");
        return;
    }

    s_mqtt_enabled   = true;
    s_mqtt_connected = false;

    Serial.printf("[MQTT] Starting — broker=%s port=%u prefix='%s'\n",
                  s_broker.c_str(), s_port, s_topic_prefix.c_str());

    xTaskCreatePinnedToCore(
        mqtt_task,
        "mqtt_task",
        8192,
        NULL,
        1,
        &s_mqtt_task_handle,
        0   // Core 0 (WiFi/network core)
    );
}

void disable_mqtt() {
    if (!s_mqtt_enabled) return;
    s_mqtt_enabled = false;
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

bool is_mqtt_enabled()   { return s_mqtt_enabled; }
bool is_mqtt_connected() { return s_mqtt_connected; }
