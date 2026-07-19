// ----------------------------------------------------------------------------
// task_network.cpp — network task lifecycle + telemetry broadcast.
// ----------------------------------------------------------------------------
// This file used to be 1000+ lines and contained EVERYTHING network-related:
// auth, Firebase upload, the WebSocket event/command dispatcher, and every
// individual command handler. It's now split into (see
// task_network_internal.h for the full map):
//   task_network.cpp     — this file: task lifecycle + broadcast*() senders
//   auth.cpp              — single-owner login
//   firebase.cpp           — Firestore token exchange + upload cycle
//   websocket.cpp          — AsyncWebSocket event/message dispatcher
//   command_handlers.cpp   — every "command" branch + pin validation
// Purely a structural split — no functionality changed.
// ----------------------------------------------------------------------------
#include "task_network.h"
#include "task_network_internal.h"
#include "state.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

unsigned long lastVitalsTime = 0;
unsigned long lastDataTime = 0;
unsigned long lastFirebaseTime = 0;

// Public entry point declared in task_network.h — see the comment there for
// why this thin wrapper exists instead of exposing wsTextAllAuthed() (defined
// in websocket.cpp) directly.
void wsBroadcastLog(const String &payload)
{
    wsTextAllAuthed(payload);
}

void initNetworkTask()
{
    webLog(0, LOG_INFO, "Initializing Network Task...");

    // 1. Wi-Fi Setup with SoftAP Fallback
    WiFi.mode(WIFI_STA);
    if (String(currentConfig.wifi_ssid).length() > 0)
    {
        WiFi.begin(currentConfig.wifi_ssid, currentConfig.wifi_pass);
        webLog(0, LOG_INFO, "Connecting to Wi-Fi: " + String(currentConfig.wifi_ssid));

        unsigned long startAttempt = millis();
        // 15-second timeout for STA
        while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000)
        {
            delay(500);
        }
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        webLog(0, LOG_WARN, "STA connection failed. Starting SoftAP fallback.");
        WiFi.mode(WIFI_AP_STA); // Keep STA active in background to allow dynamic reconnects if possible
        WiFi.softAP("HyGrow-Setup", currentConfig.ap_pass);
        webLog(0, LOG_INFO, "SoftAP IP: " + WiFi.softAPIP().toString());
    }
    else
    {
        webLog(0, LOG_INFO, "Wi-Fi Connected. IP: " + WiFi.localIP().toString());
    }

    // 2. Web Server & File System
    // NOTE: LittleFS is already mounted once in state_init() (see src/core/state.cpp),
    // and a bad mount there now halts boot before this task ever runs. Calling
    // LittleFS.begin() again here was redundant — it just re-returned true on an
    // already-mounted filesystem. We reuse the vitals flag set by state_init() instead.
    if (!currentVitals.littlefs_ok)
    {
        webLog(0, LOG_ERR, "LittleFS Mount Failed!");
    }
    else
    {
        server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    }

    // 3. Web Server & File System Setup Complete
    // (Native OTA Mount removed for attack surface reduction)

    // 4. WebSocket Mount
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // 5. Start Server
    server.begin();
    webLog(0, LOG_INFO, "Web Server started on port 80");
}

void networkTaskLoop()
{
    ws.cleanupClients();

    unsigned long currentMillis = millis();

    // Dynamic cadence for Vitals (was a hardcoded 1000ms literal — now backed
    // by currentConfig.interval_vitals_ms, same pattern as the other timers).
    if (currentMillis - lastVitalsTime >= currentConfig.interval_vitals_ms)
    {
        lastVitalsTime = currentMillis;
        broadcastVitals();
    }

    // Dynamic cadence for Data Push based on configuration
    if (currentMillis - lastDataTime >= currentConfig.interval_ws_ms)
    {
        lastDataTime = currentMillis;
        broadcastData();
    }

    // Dynamic cadence for the Firestore upload cycle. firebaseUploadCycle()
    // itself no-ops immediately if firebase_enabled/Wi-Fi/credentials aren't
    // ready, so it's safe to call unconditionally here.
    if (currentMillis - lastFirebaseTime >= currentConfig.interval_fb_ms)
    {
        lastFirebaseTime = currentMillis;
        firebaseUploadCycle();
    }
}

// (configureOtaRoutes removed)

void broadcastVitals()
{
    if (ws.count() == 0)
        return;

    JsonDocument doc;
    doc["type"] = "vitals";
    doc["rssi"] = WiFi.RSSI();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["min_free_heap"] = ESP.getMinFreeHeap();
    doc["uptime"] = millis() / 1000;
    doc["wifi_status"] = (WiFi.status() == WL_CONNECTED) ? "connected" : "ap_mode";

    // firebase_ready now reflects the outcome of the most recent real upload
    // attempt (see firebaseUploadCycle()), not just "Wi-Fi up + key present" —
    // that old proxy reported "ready" even though no upload had ever been
    // attempted. If the feature is off, report false rather than a stale
    // last-known state.
    doc["firebase_ready"] = currentConfig.firebase_enabled && currentVitals.firebase_ready;
    doc["firebase_last_ok_ms"] = currentVitals.firebase_last_ok_ms;
    doc["firebase_last_error"] = currentVitals.firebase_last_error;

    String payload;
    serializeJson(doc, payload);
    wsTextAllAuthed(payload);
}

void broadcastConfig()
{
    if (ws.count() == 0)
        return;

    JsonDocument doc;
    doc["type"] = "config";
    doc["wifi_ssid"] = currentConfig.wifi_ssid;
    doc["fb_api"] = currentConfig.fb_api_key;
    doc["fb_proj"] = currentConfig.fb_project;
    doc["fb_email"] = currentConfig.fb_email;
    doc["fb_col"] = currentConfig.fb_collection;
    doc["dev_id"] = currentConfig.device_id;
    doc["ph_off"] = currentConfig.ph_offset;
    doc["ph_slope"] = currentConfig.ph_slope;
    doc["tds_k"] = currentConfig.tds_k;

    // Feature flags — no reboot required for either of these.
    doc["demo"] = currentConfig.demo_mode;
    doc["fb_en"] = currentConfig.firebase_enabled;

    // Timing intervals (Part 5.8) — all three are already fully wired into
    // the firmware; this just exposes them for a "Timing" Settings card.
    doc["int_read"] = currentConfig.interval_read_ms;
    doc["int_ws"] = currentConfig.interval_ws_ms;
    doc["int_vit"] = currentConfig.interval_vitals_ms;
    doc["int_fb"] = currentConfig.interval_fb_ms;

    // Per-sensor enabled state. Order matches the SensorID enum in config.h
    // (S_WL, S_LIGHT, S_TDS, S_DHT, S_PH, S_WTEMP) — NOT the pin-array order
    // used by doc["pins"] below, which is ordered differently for historical
    // reasons. Keep these two arrays' orderings distinct and intentional.
    JsonArray sEn = doc["s_en"].to<JsonArray>();
    for (int i = 0; i < S_COUNT; i++)
        sEn.add(currentConfig.sensor_enabled[i]);

    // Send pins to JS UI for settings rendering
    // Order MUST match JS parser: TDS, DHT, pH, WaterTemp, WaterLevel, SDA, SCL, WaterLevelPower
    JsonArray pins = doc["pins"].to<JsonArray>();
    pins.add(currentConfig.pin_tds);
    pins.add(currentConfig.pin_dht);
    pins.add(currentConfig.pin_ph);
    pins.add(currentConfig.pin_ds18b20);
    pins.add(currentConfig.pin_wl);
    pins.add(currentConfig.pin_lux_sda);
    pins.add(currentConfig.pin_lux_scl);
    pins.add(currentConfig.pin_wl_power);

    String payload;
    serializeJson(doc, payload);
    wsTextAllAuthed(payload);
}

void broadcastData()
{
    if (ws.count() == 0)
        return;

    JsonDocument doc;
    doc["type"] = "data";
    doc["core_id_of_producer"] = 1; // Sensors run on core 1

    // Populated from the global currentSensors struct
    doc["tds"] = currentSensors.tds_ppm;
    doc["temp"] = currentSensors.temp_c;
    doc["hum"] = currentSensors.humidity;
    doc["w_t"] = currentSensors.water_temp_c;
    doc["lux"] = currentSensors.lux;
    doc["wl_percent"] = currentSensors.wl_percent;
    doc["ph_val"] = currentSensors.ph_val;
    doc["vpd_kpa"] = currentSensors.vpd_kpa;

    // Per-sensor status, one code per SensorID (same enum order as s_en[] in
    // broadcastConfig() above: S_WL, S_LIGHT, S_TDS, S_DHT, S_PH, S_WTEMP):
    //   0 = disabled (sensor_enabled[i] is false — not an error, just off)
    //   1 = healthy   (enabled, last_err[i] empty — most recent read cycle ok)
    //   2 = failing    (enabled, last_err[i] non-empty — most recent read cycle failed)
    // This mirrors exactly what sensorTaskLoop() (task_sensor.cpp) already
    // computes every cycle to drive the status LED, just serialized here too
    // instead of being LED-only. The dashboard and per-sensor detail page use
    // this to distinguish "disabled" from "enabled but not actually reading"
    // — previously neither state reached the client at all.
    JsonArray sOk = doc["s_ok"].to<JsonArray>();
    for (int i = 0; i < S_COUNT; i++)
    {
        if (!currentConfig.sensor_enabled[i])
            sOk.add(0);
        else if (currentSensors.last_err[i][0] != '\0')
            sOk.add(2);
        else
            sOk.add(1);
    }

    String payload;
    serializeJson(doc, payload);
    wsTextAllAuthed(payload);
}
