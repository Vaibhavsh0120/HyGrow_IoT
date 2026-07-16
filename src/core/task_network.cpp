/*
 * ============================================================================
 * task_network.cpp — WiFi, WebServer, WebSockets, & Firebase (Core 0)
 * ============================================================================
 */

#include "task_network.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>    // Install via Library Manager
#include <FirebaseClient.h> // Install via Library Manager

#include "state.h"
#include "../../secrets.h"
#include "../../config.h"

// ── WebServer & WebSockets ──
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ── Firebase Objects ──
WiFiClientSecure sslClient;
DefaultNetwork network;
AsyncClientClass aClient(sslClient, getNetwork(network));
FirebaseApp app;
Firestore::Documents Docs;
UserAuth userAuth(SECRET_FIREBASE_API_KEY, SECRET_FIREBASE_USER_EMAIL, SECRET_FIREBASE_USER_PASSWORD);
AsyncResult fbResult;

// ── Broadcast Log ──
void broadcastLog(String msg) {
    // Send standard log wrapper over WebSockets
    String jsonLog = "{\"type\":\"log\",\"msg\":\"" + msg + "\"}";
    ws.textAll(jsonLog);
}

// ── WebSocket Event Handler ──
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            data[len] = 0;
            String msg = (char*)data;

            // Parse incoming JSON from Web UI
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, msg);
            if (!error) {
                if (doc["cmd"] == "toggle_sensor") {
                    int s_id = doc["id"];
                    bool is_on = doc["state"];

                    if (s_id >= 0 && s_id < S_COUNT) {
                        if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
                            currentConfig.sensor_enabled[s_id] = is_on;
                            state_save(); // Save to NVS flash
                            xSemaphoreGive(stateMutex);
                        }
                        webLog("[UI] Sensor %d toggled %s", s_id, is_on ? "ON" : "OFF");
                    }
                }
                else if (doc["cmd"] == "toggle_demo") {
                    bool is_demo = doc["state"];
                    if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
                        currentConfig.demo_mode = is_demo;
                        state_save();
                        xSemaphoreGive(stateMutex);
                    }
                    webLog("[UI] Demo Mode turned %s", is_demo ? "ON" : "OFF");
                }
            }
        }
    }
}

// ── Main Task Loop (Core 0) ──
void network_task_loop(void *pvParameters) {
    // 1. WiFi Initialization
    WiFi.mode(WIFI_STA);
    WiFi.begin(currentConfig.wifi_ssid, currentConfig.wifi_password);

    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiStart < 15000)) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("\n>>> WEB DIAGNOSE READY: http://");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\n>>> WIFI FAILED - Connect to Web UI via fallback AP or re-configure via serial.");
        // Advanced: You could launch an Access Point here if WiFi fails
    }

    // 2. Initialize LittleFS
    if (!LittleFS.begin(true)) {
        Serial.println("CRITICAL: LittleFS Mount Failed! Web UI will not load.");
    } else {
        // Serve all static files in the /data folder automatically!
        server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    }

    // 3. Initialize WebSockets & Server
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.begin();

    // 4. Initialize Firebase
    sslClient.setInsecure(); // Skip certificate checking for simplicity
    initializeApp(aClient, app, getAuth(userAuth), fbResult);
    app.getApp<Firestore::Documents>(Docs);

    unsigned long lastFbSend = 0;
    unsigned long lastWsUpdate = 0;

    // 5. Infinite Core 0 Loop
    while (true) {
        // Must be called continuously to maintain Firebase connection and async tasks
        app.loop();
        ws.cleanupClients();

        unsigned long now = millis();

        // ── Real-Time WebSockets Update (Every 1 Second) ──
        if (now - lastWsUpdate > 1000) {
            lastWsUpdate = now;

            ConfigState cfg; SensorData data;
            if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
                cfg = currentConfig; data = currentData;
                xSemaphoreGive(stateMutex);
            }

            // Build lightweight JSON for the Dashboard graphs
            JsonDocument wsDoc;
            wsDoc["type"] = "data";
            wsDoc["tds"]  = data.tds_ppm;
            wsDoc["temp"] = data.dht_temp;
            wsDoc["hum"]  = data.dht_hum;
            wsDoc["w_t"]  = data.w_temp;
            wsDoc["lux"]  = data.light_lux;

            String wsStr;
            serializeJson(wsDoc, wsStr);
            ws.textAll(wsStr);
        }

        // ── Firebase Upload (Every 10 Seconds) ──
        if (now - lastFbSend > 10000 && app.ready()) {
            lastFbSend = now;

            ConfigState cfg; SensorData data;
            if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
                cfg = currentConfig; data = currentData;
                xSemaphoreGive(stateMutex);
            }

            // DYNAMIC FIREBASE PAYLOAD: Only add keys if the sensor is ON
            Document<Values::Value> doc;

            if (cfg.sensor_enabled[S_TDS])   doc.add("tds_ppm", Values::Value(Values::DoubleValue(data.tds_ppm)));
            if (cfg.sensor_enabled[S_LIGHT]) doc.add("light_lux", Values::Value(Values::DoubleValue(data.light_lux)));
            if (cfg.sensor_enabled[S_WTEMP]) doc.add("water_temp_c", Values::Value(Values::DoubleValue(data.w_temp)));

            if (cfg.sensor_enabled[S_DHT]) {
                doc.add("air_temp_c", Values::Value(Values::DoubleValue(data.dht_temp)));
                doc.add("humidity_percent", Values::Value(Values::DoubleValue(data.dht_hum)));
                doc.add("vpd_kpa", Values::Value(Values::DoubleValue(data.vpd_kpa)));
            }

            // Incomplete Sensors: Will only be pushed if the user stubbornly turns them ON in the UI
            if (cfg.sensor_enabled[S_WL]) doc.add("water_level_pct", Values::Value(Values::DoubleValue(data.wl_percent)));
            if (cfg.sensor_enabled[S_PH]) doc.add("ph_value", Values::Value(Values::DoubleValue(data.ph_val)));

            String docPath = String(FIRESTORE_COLLECTION) + "/" + DEVICE_ID;

            // Patch document (Create or Update)
            Docs.patch(aClient, Firestore::Parent(SECRET_FIREBASE_PROJECT_ID), docPath,
                       PatchDocumentOptions(DocumentMask(), DocumentMask(), Precondition()), doc, fbResult);

            webLog("[FIREBASE] Payload Synced");
        }

        // Yield 10ms to the underlying FreeRTOS networking stack
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
