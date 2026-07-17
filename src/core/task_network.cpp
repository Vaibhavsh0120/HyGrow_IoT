#include "task_network.h"
#include "state.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ElegantOTA.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

unsigned long lastVitalsTime = 0;
unsigned long lastDataTime = 0;

// Internal helpers
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len);
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);

void initNetworkTask() {
    webLog(0, "info", "Initializing Network Task...");

    // 1. Wi-Fi Setup with SoftAP Fallback
    WiFi.mode(WIFI_STA);
    if (String(currentConfig.wifi_ssid).length() > 0) {
        WiFi.begin(currentConfig.wifi_ssid, currentConfig.wifi_pass);
        webLog(0, "info", "Connecting to Wi-Fi: " + String(currentConfig.wifi_ssid));

        unsigned long startAttempt = millis();
        // 15-second timeout for STA
        while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
            delay(500);
        }
    }

    if (WiFi.status() != WL_CONNECTED) {
        webLog(0, "warn", "STA connection failed. Starting SoftAP fallback.");
        WiFi.mode(WIFI_AP_STA); // Keep STA active in background to allow dynamic reconnects if possible
        WiFi.softAP("HyGrow-Setup", currentConfig.ap_pass);
        webLog(0, "info", "SoftAP IP: " + WiFi.softAPIP().toString());
    } else {
        webLog(0, "info", "Wi-Fi Connected. IP: " + WiFi.localIP().toString());
    }

    // 2. Web Server & File System Mount
    if (!LittleFS.begin()) {
        webLog(0, "error", "LittleFS Mount Failed!");
    } else {
        server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    }

    // 3. ElegantOTA Mount
    ElegantOTA.begin(&server);
    ElegantOTA.onStart([]() { webLog(0, "info", "OTA Update Started"); });
    ElegantOTA.onProgress([](size_t current, size_t final) {
        // Throttle logs to prevent WS flood, log every 10%
        static int lastPercent = 0;
        int percent = (current * 100) / final;
        if (percent - lastPercent >= 10) {
            webLog(0, "info", "OTA Progress: " + String(percent) + "%");
            lastPercent = percent;
        }
    });
    ElegantOTA.onEnd([](bool success) {
        if(success) webLog(0, "info", "OTA Update Complete. Rebooting...");
        else webLog(0, "error", "OTA Update Failed");
    });

    // 4. WebSocket Mount
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // 5. Start Server
    server.begin();
    webLog(0, "info", "Web Server started on port 80");
}

void networkTaskLoop() {
    ElegantOTA.loop();
    ws.cleanupClients();

    unsigned long currentMillis = millis();

    // 1-second cadence for Vitals
    if (currentMillis - lastVitalsTime >= 1000) {
        lastVitalsTime = currentMillis;
        broadcastVitals();
    }

    // Dynamic cadence for Data Push based on currentConfig.int_ws
    if (currentMillis - lastDataTime >= currentConfig.int_ws) {
        lastDataTime = currentMillis;
        broadcastData();
    }
}

void broadcastVitals() {
    if (ws.count() == 0) return;

    StaticJsonDocument<256> doc;
    doc["type"] = "vitals";
    doc["rssi"] = WiFi.RSSI();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["min_free_heap"] = ESP.getMinFreeHeap();
    doc["uptime"] = millis() / 1000;
    doc["wifi_status"] = (WiFi.status() == WL_CONNECTED) ? "connected" : "ap_mode";
    // Placeholder for Firebase state; wire to your actual Firebase client status
    doc["firebase_ready"] = (WiFi.status() == WL_CONNECTED && String(currentConfig.fb_api).length() > 0);

    String payload;
    serializeJson(doc, payload);
    ws.textAll(payload);
}

void broadcastConfig() {
    if (ws.count() == 0) return;

    DynamicJsonDocument doc(1024);
    doc["type"] = "config";
    doc["wifi_ssid"] = currentConfig.wifi_ssid;
    doc["fb_api"] = currentConfig.fb_api;
    doc["fb_proj"] = currentConfig.fb_proj;
    doc["fb_email"] = currentConfig.fb_email;
    doc["fb_col"] = currentConfig.fb_col;
    doc["dev_id"] = currentConfig.dev_id;
    doc["ph_off"] = currentConfig.ph_offset;
    doc["ph_slope"] = currentConfig.ph_slope;
    doc["tds_k"] = currentConfig.tds_k;
    doc["int_read"] = currentConfig.int_read;
    doc["int_ws"] = currentConfig.int_ws;
    doc["int_fb"] = currentConfig.int_fb;

    // Send pins to UI for settings rendering
    JsonArray pins = doc.createNestedArray("pins");
    pins.add(currentConfig.pin_tds);
    pins.add(currentConfig.pin_ph);
    // Add other pins here as needed...

    String payload;
    serializeJson(doc, payload);
    ws.textAll(payload);
}

void broadcastData() {
    if (ws.count() == 0) return;

    StaticJsonDocument<512> doc;
    doc["type"] = "data";
    doc["core_id_of_producer"] = 1; // Sensors run on core 1

    // Assuming a global currentData struct populated by the sensor task
    doc["tds"] = currentData.tds;
    doc["temp"] = currentData.air_temp;
    doc["hum"] = currentData.air_hum;
    doc["w_t"] = currentData.water_temp;
    doc["lux"] = currentData.lux;

    // UI required fields
    doc["wl_percent"] = currentData.wl_percent;
    doc["ph_val"] = currentData.ph_val;
    doc["vpd_kpa"] = currentData.vpd_kpa;

    JsonArray sensors_enabled = doc.createNestedArray("sensor_enabled");
    sensors_enabled.add(currentConfig.pin_tds >= 0);
    sensors_enabled.add(currentConfig.pin_ph >= 0);
    // Add logic for other sensors

    JsonArray errors = doc.createNestedArray("errors");
    // Add error logic if hardware reads fail (e.g., NaN returned)

    String payload;
    serializeJson(doc, payload);
    ws.textAll(payload);
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        webLog(0, "info", "WS Client Connected: " + String(client->id()));
        broadcastConfig(); // Sync UI immediately
    } else if (type == WS_EVT_DISCONNECT) {
        webLog(0, "info", "WS Client Disconnected: " + String(client->id()));
    } else if (type == WS_EVT_DATA) {
        handleWebSocketMessage(arg, data, len);
    }
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        data[len] = 0;
        String msg = (char*)data;

        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, msg);

        if (err) {
            webLog(0, "error", "WS Parse Error: " + String(err.c_str()));
            return;
        }

        String cmd = doc["command"].as<String>();

        if (cmd == "save_wifi") {
            strlcpy(currentConfig.wifi_ssid, doc["ssid"] | "", sizeof(currentConfig.wifi_ssid));
            strlcpy(currentConfig.wifi_pass, doc["pass"] | "", sizeof(currentConfig.wifi_pass));
            saveConfig();
            broadcastConfig();
            webLog(0, "info", "WiFi config saved. Reboot to apply.");
        }
        else if (cmd == "save_firebase") {
            strlcpy(currentConfig.fb_api, doc["api"] | "", sizeof(currentConfig.fb_api));
            strlcpy(currentConfig.fb_proj, doc["proj"] | "", sizeof(currentConfig.fb_proj));
            strlcpy(currentConfig.fb_email, doc["email"] | "", sizeof(currentConfig.fb_email));
            strlcpy(currentConfig.fb_pass, doc["pass"] | "", sizeof(currentConfig.fb_pass));
            strlcpy(currentConfig.fb_col, doc["col"] | "", sizeof(currentConfig.fb_col));
            saveConfig();
            broadcastConfig();
            webLog(0, "info", "Firebase config updated.");
            // Add Firebase re-init call here if doing hot-swaps
        }
        else if (cmd == "calibrate_ph") {
            currentConfig.ph_offset = doc["offset"] | currentConfig.ph_offset;
            currentConfig.ph_slope = doc["slope"] | currentConfig.ph_slope;
            saveConfig();
            broadcastConfig();
            webLog(0, "info", "pH Calibration saved.");
        }
        else if (cmd == "calibrate_tds") {
            currentConfig.tds_k = doc["tds_k"] | currentConfig.tds_k;
            saveConfig();
            broadcastConfig();
            webLog(0, "info", "TDS Calibration saved.");
        }
        else if (cmd == "factory_reset") {
            webLog(0, "warn", "Factory reset initiated. Wiping NVS...");
            Preferences prefs;
            prefs.begin("hygrow", false);
            prefs.clear();
            prefs.end();
            webLog(0, "warn", "NVS wiped. Rebooting in 2s...");
            delay(2000);
            ESP.restart();
        }
        else if (cmd == "reboot") {
            webLog(0, "warn", "Manual reboot requested...");
            delay(1000);
            ESP.restart();
        }
        else if (cmd == "request_vitals") {
            broadcastVitals();
        }
    }
}
