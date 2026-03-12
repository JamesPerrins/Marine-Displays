#include "signalk_config.h"
#include "network_setup.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <vector>

// ── mbedTLS PSRAM allocator (linker-wrap) ────────────────────────────────────
// Internal mbedTLS code calls esp_mbedtls_mem_calloc() directly via
// MBEDTLS_PLATFORM_CALLOC_MACRO — bypassing mbedtls_platform_set_calloc_free().
// Wrapping esp_mbedtls_mem_calloc at link time intercepts ALL mbedTLS heap
// allocations, including in the pre-compiled libmbedcrypto.a.
// Blocks ≥4KB (the two 16KB SSL record buffers) go to PSRAM, leaving the ~30KB
// iRAM for SSL context structs and certificate chain parsing.
// Build flag: -Wl,--wrap=esp_mbedtls_mem_calloc -Wl,--wrap=esp_mbedtls_mem_free
extern "C" void* __real_esp_mbedtls_mem_calloc(size_t n, size_t size);
extern "C" void  __real_esp_mbedtls_mem_free(void* ptr);

extern "C" void* __wrap_esp_mbedtls_mem_calloc(size_t n, size_t size) {
    size_t total = n * size;
    if (total >= 4096) {
        void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) return p;
        // PSRAM full: fall through to internal RAM
    }
    return __real_esp_mbedtls_mem_calloc(n, size);
}

extern "C" void __wrap_esp_mbedtls_mem_free(void* ptr) {
    heap_caps_free(ptr);
}
// ─────────────────────────────────────────────────────────────────────────────

// Global array to hold all sensor values (10 parameters)
float g_sensor_values[TOTAL_PARAMS] = {
    0,        // SCREEN1_RPM
    313.15,   // SCREEN1_COOLANT_TEMP
    0,        // SCREEN2_RPM
    50.0,     // SCREEN2_FUEL
    313.15,   // SCREEN3_COOLANT_TEMP
    373.15,   // SCREEN3_EXHAUST_TEMP
    50.0,     // SCREEN4_FUEL
    313.15,   // SCREEN4_COOLANT_TEMP
    2.0,      // SCREEN5_OIL_PRESSURE
    313.15    // SCREEN5_COOLANT_TEMP
};

// Mutex for thread-safe access to sensor variables
SemaphoreHandle_t sensor_mutex = NULL;

// WiFi and HTTP client (static to this file)
static WebSocketsClient ws_client;
static String server_ip_str = "";
static uint16_t server_port_num = 0;
static bool use_ssl = false;
static String signalk_paths[TOTAL_PARAMS];  // Array of 10 paths
static TaskHandle_t signalk_task_handle = NULL;
static bool signalk_enabled = false;

// Set by HTTP handler (Core 1) before building/sending the config page.
// signalk_task (Core 0) sees this, disconnects the WS, and suspends reconnects
// until the flag is cleared on resume — freeing the ~22KB WS receive buffer.
static volatile bool g_signalk_ws_paused = false;
// Set by resume_signalk_ws() to tell signalk_task to reconnect once the flag clears.
static volatile bool g_signalk_ws_resume_when_ready = false;

// Persistent buffer for setExtraHeaders — must outlive ws_connect() so the
// WebSocket library can safely read it when sending the HTTP upgrade request.
static String ws_extra_headers;

// Cached subscription path list — populated at connect time so the TEXT
// handler can filter incoming values to only subscribed paths, preventing
// the server's full-state broadcast from filling memory with unsubscribed paths.
static std::vector<String> s_subscribed_paths;

// Timestamp (millis) of the most recent successful data update via update_signalk_value().
// 0 = no data ever received.  Read from any task via get_last_data_update_ms().
static volatile uint32_t s_last_any_update_ms = 0;

uint32_t get_last_data_update_ms() { return s_last_any_update_ms; }

// Connection health and reconnection/backoff state
static unsigned long last_message_time = 0;
static unsigned long last_reconnect_attempt = 0;
static unsigned long next_reconnect_at = 0;
static unsigned long current_backoff_ms = 2000; // start 2s
static const unsigned long RECONNECT_BASE_MS = 2000;
static const unsigned long RECONNECT_MAX_MS = 60000;
static const unsigned long MESSAGE_TIMEOUT_MS = 120000; // 120s without messages => reconnect
static const unsigned long PING_INTERVAL_MS = 15000; // send periodic ping

// Outgoing message queue (simple ring buffer)
static SemaphoreHandle_t ws_queue_mutex = NULL;
static const int OUTGOING_QUEUE_SIZE = 8;
static String outgoing_queue[OUTGOING_QUEUE_SIZE];
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;

static bool enqueue_outgoing(const String &msg) {
    if (ws_queue_mutex == NULL) return false;
    if (xSemaphoreTake(ws_queue_mutex, pdMS_TO_TICKS(100))) {
        if (queue_count >= OUTGOING_QUEUE_SIZE) {
            // Drop oldest to make room
            queue_head = (queue_head + 1) % OUTGOING_QUEUE_SIZE;
            queue_count--;
        }
        outgoing_queue[queue_tail] = msg;
        queue_tail = (queue_tail + 1) % OUTGOING_QUEUE_SIZE;
        queue_count++;
        xSemaphoreGive(ws_queue_mutex);
        return true;
    }
    return false;
}

static void flush_outgoing() {
    if (ws_queue_mutex == NULL) return;
    if (!ws_client.isConnected()) return;
    if (!xSemaphoreTake(ws_queue_mutex, pdMS_TO_TICKS(100))) return;
    while (queue_count > 0 && ws_client.isConnected()) {
        String &m = outgoing_queue[queue_head];
        ws_client.sendTXT(m);
        queue_head = (queue_head + 1) % OUTGOING_QUEUE_SIZE;
        queue_count--;
    }
    xSemaphoreGive(ws_queue_mutex);
}

// Public enqueue wrapper (declared in header)
void enqueue_signalk_message(const String &msg) {
    if (ws_queue_mutex == NULL) return;
    enqueue_outgoing(msg);
}

static void wsEvent(WStype_t type, uint8_t * payload, size_t length); // forward declaration

// Connect (or reconnect) WebSocket, using SSL when port is 443
static void ws_connect() {
    // Always disconnect first so any pending TCP/SSL socket is closed cleanly
    // before we reinitialise. Without this, repeated beginSSL() calls abandon
    // in-progress handshakes without freeing the SSL context, leaking heap.
    ws_client.disconnect();
    if (use_ssl) {
        // Check DNS resolves before attempting SSL (fast failure diagnostic)
        IPAddress resolved;
        bool dns_ok = WiFi.hostByName(server_ip_str.c_str(), resolved);
        printf("[SK] DNS: %s -> %s (%s)  iRAM=%u\n",
            server_ip_str.c_str(), resolved.toString().c_str(),
            dns_ok ? "OK" : "FAIL",
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        // NULL CA cert + NULL fingerprint causes the library to call setInsecure()
        // internally, accepting self-signed/Cloudflare certificates.
        // ?subscribe=none tells the server to start with zero default subscriptions.
        // Without this the server pushes ALL known paths on connect; our explicit
        // subscribe message then becomes the only active subscription.
        ws_client.beginSSL(server_ip_str.c_str(), server_port_num, "/signalk/v1/stream?subscribe=none");
        printf("Signal K: connecting via WSS (SSL)\n");
    } else {
        ws_client.begin(server_ip_str.c_str(), server_port_num, "/signalk/v1/stream?subscribe=none");
        printf("Signal K: connecting via WS (plain)\n");
    }
    ws_client.onEvent(wsEvent);
    ws_client.setReconnectInterval(0);  // disable library reconnect - we manage it ourselves
    // Build extra headers: Origin always, plus CF Access tokens if configured.
    // ws_extra_headers is file-scope static so the pointer stays valid after
    // ws_connect() returns — the WebSocket library reads it lazily on loop().
    String origin = String(use_ssl ? "https://" : "http://") + server_ip_str;
    ws_extra_headers = "Origin: " + origin;
    String cf_id = get_cf_client_id();
    String cf_secret = get_cf_client_secret();
    if (cf_id.length() > 0 && cf_secret.length() > 0) {
        ws_extra_headers += "\r\nCF-Access-Client-Id: " + cf_id;
        ws_extra_headers += "\r\nCF-Access-Client-Secret: " + cf_secret;
        printf("Signal K: CF Access headers added\n");
    }
    ws_client.setExtraHeaders(ws_extra_headers.c_str());
}

// Convert dot-delimited Signal K path to REST URL form
static String build_signalk_url(const String &path) {
    String cleaned = path;
    cleaned.trim();
    cleaned.replace(".", "/");
    return String("/signalk/v1/api/vessels/self/") + cleaned;
}

// Thread-safe getter for any sensor value
float get_sensor_value(int index) {
    if (index < 0 || index >= TOTAL_PARAMS) return 0;

    float val = 0;
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        val = g_sensor_values[index];
        xSemaphoreGive(sensor_mutex);
    }
    return val;
}

// Thread-safe setter for any sensor value
void set_sensor_value(int index, float value) {
    if (index < 0 || index >= TOTAL_PARAMS) return;

    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        g_sensor_values[index] = value;
        xSemaphoreGive(sensor_mutex);
    }
}

// Initialize mutex
void init_sensor_mutex() {
    if (sensor_mutex == NULL) {
        sensor_mutex = xSemaphoreCreateMutex();
    }
}

// Route an incoming path+value to the correct sensor slot(s).
// Safe to call from any task/core; uses the sensor mutex internally.
// Used by MQTT and any future data sources as a common update entry point.
void update_signalk_value(const char* path, float value) {
    if (!path || path[0] == '\0') return;
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i].length() > 0 && signalk_paths[i].equals(path)) {
            set_sensor_value(i, value);
            s_last_any_update_ms = (uint32_t)millis();
        }
    }
}

// WebSocket event handler
static void wsEvent(WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_CONNECTED) {
        printf("Signal K: WebSocket connected  iRAM=%u PSRAM=%u\n",
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        last_message_time = millis();
        // reset backoff on successful connect
        current_backoff_ms = RECONNECT_BASE_MS;

        // Build subscription JSON manually to avoid DynamicJsonDocument heap alloc.
        // Populate s_subscribed_paths cache so TEXT handler can filter efficiently.
        s_subscribed_paths.clear();
        String out = "{\"context\":\"vessels.self\",\"subscribe\":[";
        bool first_conn = true;
        printf("[SK] Subscribing to %d paths:\n", TOTAL_PARAMS);
        for (int i = 0; i < TOTAL_PARAMS; i++) {
            if (signalk_paths[i].length() > 0) {
                printf("[SK]   %s\n", signalk_paths[i].c_str());
                s_subscribed_paths.push_back(signalk_paths[i]);
                if (!first_conn) out += ",";
                out += "{\"path\":\"";
                out += signalk_paths[i];
                out += "\",\"period\":2000}";
                first_conn = false;
            }
        }
        out += "]}";
        if (first_conn) {
            printf("[SK WARN] No paths to subscribe — check gauge/screen config\n");
        }
        // Enqueue rather than calling sendTXT() directly from inside the wsEvent callback
        // (which is called from ws_client.loop()). With SSL, writing from within a read
        // callback can silently fail. flush_outgoing() in the task loop will send it
        // on the very next iteration, once loop() has returned cleanly.
        enqueue_outgoing(out);
        return;
    }

    if (type == WStype_TEXT) {
        last_message_time = millis();

        // Log the first few messages in full so we can see what the server sends.
        static int msg_count = 0;
        msg_count++;
        if (msg_count <= 5) {
            int preview = (length < 160) ? (int)length : 160;
            printf("[SK MSG #%d len=%u] %.*s%s\n",
                msg_count, (unsigned)length, preview, (const char*)payload,
                (length > 160 ? "..." : ""));
        }

        // Periodic iRAM diagnostic: log every 30s to catch slow leaks.
        static unsigned long last_mem_log = 0;
        unsigned long now_ms = millis();
        if (now_ms - last_mem_log >= 30000) {
            last_mem_log = now_ms;
            printf("[SK MEM] iRAM=%u PSRAM=%u msg_len=%u total_msgs=%d\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)length, msg_count);
        }
        // Warn if iRAM drops below 15 KB — crash is likely imminent.
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 15360) {
            printf("[SK WARN] LOW iRAM=%u PSRAM=%u\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }

        // Parse incoming JSON and look for updates->values.
        // Use 8192-byte capacity so malloc(8192) > SPIRAM_MALLOC_ALWAYSINTERNAL(4096)
        // threshold → pool allocated from PSRAM instead of internal RAM.
        DynamicJsonDocument doc(8192);
        DeserializationError err = deserializeJson(doc, (const char*)payload, length);
        if (err) {
            printf("[SIGNALK] JSON parse error: %s\n", err.c_str());
            return;
        }

        if (doc.containsKey("updates")) {
            JsonArray updates = doc["updates"].as<JsonArray>();
            for (JsonVariant update : updates) {
                if (!update.containsKey("values")) continue;
                JsonArray values = update["values"].as<JsonArray>();
                for (JsonVariant val : values) {
                    if (!val.containsKey("path") || !val.containsKey("value")) continue;
                    const char* path = val["path"];
                    float value = val["value"].as<float>();

                    // Log first 20 value updates so we can confirm data is flowing
                    static int val_log_count = 0;
                    if (val_log_count < 20) {
                        val_log_count++;
                        printf("[SK VAL] %s = %.4f\n", path, value);
                    }

                    // Check path against all configured gauge slots and update.
                    // No s_subscribed_paths filter here: the Round Display has no extended
                    // sensor map, so unmatched paths simply don't update anything. This also
                    // handles the case where refresh_signalk_subscriptions() resends the
                    // subscription before s_subscribed_paths is refreshed via a reconnect.
                    for (int i = 0; i < TOTAL_PARAMS; i++) {
                        if (signalk_paths[i].length() > 0 && signalk_paths[i].equals(path)) {
                            set_sensor_value(i, value);
                        }
                    }
                }
            }
        }
    }

    if (type == WStype_PONG) {
        last_message_time = millis();
    }
    if (type == WStype_DISCONNECTED) {
        printf("[SK] Disconnected  iRAM=%u PSRAM=%u\n",
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    if (type == WStype_ERROR) {
        printf("[SK] WS error  iRAM=%u  detail=%.*s\n",
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (int)length, payload ? (char*)payload : "(none)");
    }
}

// FreeRTOS task for Signal K updates (runs on core 0)
static void signalk_task(void *parameter) {
    printf("Signal K WebSocket task started\n");
    vTaskDelay(pdMS_TO_TICKS(500));

    while (signalk_enabled) {
        // Config UI pause: the HTTP handler (Core 1) sets g_signalk_ws_paused before
        // building/sending the config page. We disconnect here (Core 0, the only
        // thread that safely owns ws_client) to free the WS receive buffer.
        if (g_signalk_ws_paused) {
            if (ws_client.isConnected()) {
                ws_client.disconnect();
                current_backoff_ms = RECONNECT_BASE_MS;
                printf("[SK] Config UI active - WS disconnected to free iRAM\n");
            }
            if (g_signalk_ws_resume_when_ready) {
                g_signalk_ws_resume_when_ready = false;
                g_signalk_ws_paused = false;
                next_reconnect_at = millis() + 1000;
                printf("[SK] WS unpaused, reconnecting in 1s\n");
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ws_client.loop();
        // Drain any messages queued from other tasks (e.g. refresh_signalk_subscriptions
        // called from the HTTP handler on Core 1). Only safe to call sendTXT() from here.
        flush_outgoing();

        unsigned long now = millis();

        // send periodic ping if connected
        if (ws_client.isConnected()) {
            if (now - last_message_time >= PING_INTERVAL_MS) {
                ws_client.sendPing();
            }
        }

        // detect silent drop: no messages/pongs for MESSAGE_TIMEOUT_MS
        if (ws_client.isConnected()) {
            if (now - last_message_time >= MESSAGE_TIMEOUT_MS) {
                printf("Signal K: connection idle timeout, forcing disconnect\n");
                ws_client.disconnect();
                unsigned int jitter = (esp_random() & 0x7FF) % 1000;
                next_reconnect_at = now + current_backoff_ms + jitter;
                last_reconnect_attempt = now;
                current_backoff_ms = min(current_backoff_ms * 2, RECONNECT_MAX_MS);
            }
        } else {
            // Not connected: attempt reconnect when scheduled
            if (next_reconnect_at == 0) {
                // First pass: enable_signalk() already called ws_connect() before
                // the task started. Give the SSL+WebSocket handshake 10s to
                // complete (Cloudflare TLS takes 3-5s) before we declare failure
                // and restart. A 2s window caused repeated beginSSL() calls that
                // abandoned in-progress handshakes and leaked the SSL context.
                next_reconnect_at = now + 10000;
            }
            if (now >= next_reconnect_at) {
                ws_connect();
                last_reconnect_attempt = now;
                unsigned int jitter = (esp_random() & 0x7FF) % 1000;
                next_reconnect_at = now + current_backoff_ms + jitter;
                current_backoff_ms = min(current_backoff_ms * 2, RECONNECT_MAX_MS);
            }
        }

        // Log stack high-water-mark every 15s to detect stack overflow risk
        static unsigned long last_hwm_log = 0;
        if (millis() - last_hwm_log >= 15000) {
            last_hwm_log = millis();
            printf("[SK] Stack HWM: %u bytes free\n", uxTaskGetStackHighWaterMark(NULL));
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    printf("Signal K WebSocket task ended\n");
    vTaskDelete(NULL);
}

// Enable Signal K with WiFi credentials
void enable_signalk(const char* ssid, const char* password, const char* server_ip, uint16_t server_port) {
    if (signalk_enabled) {
        printf("Signal K already enabled\n");
        return;
    }

    signalk_enabled = true;
    server_ip_str = server_ip;
    server_port_num = server_port;
    use_ssl = (server_port == 443);

    // Get all 10 paths from configuration
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        signalk_paths[i] = get_signalk_path_by_index(i);
        printf("=== ACTIVE Signal K path[%d] = '%s' ===\n", i, signalk_paths[i].c_str());
    }

    printf("=== Signal K paths loaded from configuration ===\n");

    // Initialize mutex first
    init_sensor_mutex();
    if (ws_queue_mutex == NULL) {
        ws_queue_mutex = xSemaphoreCreateMutex();
    }

    if (WiFi.status() != WL_CONNECTED) {
        printf("Signal K: WiFi not connected, aborting\n");
        signalk_enabled = false;
        return;
    }
    printf("Signal K: Starting WebSocket client...\n");

    ws_connect();

    // FreeRTOS requires task stacks in internal RAM. Use xTaskCreatePinnedToCore
    // so the stack is allocated via pvPortMalloc which draws from internal heap.
    xTaskCreatePinnedToCore(
        signalk_task,
        "SignalKWS",
        16384,  // SSL requires ~12-16KB stack
        NULL,
        3,
        &signalk_task_handle,
        0
    );
    if (signalk_task_handle == NULL) {
        printf("[SK] Failed to create SignalK task - out of internal RAM\n");
    }

    printf("Signal K WebSocket task created successfully\n");
}

// Disable Signal K
void disable_signalk() {
    signalk_enabled = false;
    if (signalk_task_handle != NULL) {
        vTaskDelete(signalk_task_handle);
        signalk_task_handle = NULL;
    }
    ws_client.disconnect();
    printf("Signal K disabled (WebSocket disconnected)\n");
}

// Returns true if the WS is currently paused.
bool is_signalk_ws_paused() {
    return g_signalk_ws_paused;
}

// Pause the WebSocket connection while the config UI is open.
// Sets the pause flag and yields 300ms so signalk_task (Core 0) sees it,
// calls ws_client.disconnect(), and the WS receive buffer is freed
// before the HTTP handler builds and sends the large config page.
void pause_signalk_ws() {
    if (!signalk_enabled) return;
    g_signalk_ws_paused = true;
    for (int i = 0; i < 6; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_task_wdt_reset();
    }
    printf("[SK] WS paused for config UI, iRAM now %u B\n",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

// Resume the WebSocket connection after the config save completes.
void resume_signalk_ws() {
    if (!signalk_enabled) return;
    g_signalk_ws_resume_when_ready = true;
    printf("[SK] WS resume requested\n");
}

// Sync configured paths into the local table used by update_signalk_value().
// Called at startup for MQTT mode (which skips enable_signalk/refresh).
void load_signalk_paths() {
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        signalk_paths[i] = get_signalk_path_by_index(i);
    }
    printf("[SignalK] paths loaded for MQTT: '%s'\n", signalk_paths[0].c_str());
}

// Rebuild the subscription list from current configuration and (re)send it
// over the active WebSocket connection if connected.
void refresh_signalk_subscriptions() {
    // Reload signalk_paths from configuration
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        signalk_paths[i] = get_signalk_path_by_index(i);
        printf("[SignalK] refreshed path[%d] = '%s'\n", i, signalk_paths[i].c_str());
    }
    // Keep s_subscribed_paths in sync so the CONNECTED handler logging is accurate
    // on the next reconnect, and so any future filter uses the latest paths.
    s_subscribed_paths.clear();
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i].length() > 0) s_subscribed_paths.push_back(signalk_paths[i]);
    }

    // Build subscription JSON manually to avoid DynamicJsonDocument allocating
    // 2048 bytes from internal iRAM. Manual string building uses PSRAM-backed
    // Arduino String objects instead.
    String out = "{\"context\":\"vessels.self\",\"subscribe\":[";
    bool first = true;
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i].length() > 0) {
            if (!first) out += ",";
            out += "{\"path\":\"";
            out += signalk_paths[i];
            out += "\",\"period\":2000}";
            first = false;
        }
    }
    out += "]}";

    // Always queue — never call ws_client.sendTXT() directly here.
    // refresh_signalk_subscriptions() may be called from Core 1 (HTTP handler)
    // while signalk_task on Core 0 is inside ws_client.loop(). Calling sendTXT()
    // from two cores simultaneously is an unprotected race that crashes the device.
    // flush_outgoing() inside signalk_task will drain the queue safely from Core 0.
    if (enqueue_outgoing(out)) {
        printf("[SignalK] subscription payload queued\n");
    } else {
        printf("[SignalK] failed to queue subscription payload\n");
    }
}