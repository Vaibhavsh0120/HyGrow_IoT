#include "task_network.h"
#include "state.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

static const char OTA_PAGE[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>HyGrow OTA Update</title>
    <style>
        :root { color-scheme: dark; font-family: system-ui, sans-serif; }
        body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #0b1220; color: #fff; }
        .card { width: min(92vw, 560px); padding: 24px; border: 1px solid rgba(255,255,255,.12); border-radius: 20px; background: rgba(255,255,255,.06); backdrop-filter: blur(18px); box-shadow: 0 20px 60px rgba(0,0,0,.35); }
        h1 { margin: 0 0 12px; font-size: 1.6rem; }
        p { margin: 0 0 16px; line-height: 1.5; opacity: .88; }
        input, button { width: 100%; box-sizing: border-box; border-radius: 14px; border: 1px solid rgba(255,255,255,.14); padding: 14px 16px; font: inherit; }
        input { color: #fff; background: rgba(255,255,255,.06); margin-bottom: 12px; }
        button { border: 0; background: linear-gradient(135deg, #4f9cff, #6fe7c8); color: #08111d; font-weight: 700; cursor: pointer; }
        small { display: block; margin-top: 12px; opacity: .72; }
    </style>
</head>
<body>
    <div class="card">
        <h1>HyGrow Firmware Update</h1>
        <p>Select a compiled <strong>.bin</strong> file and upload it to the ESP32-S3. The device will reboot automatically after the transfer completes.</p>
        <form method="POST" action="/ota/upload" enctype="multipart/form-data">
            <input type="file" name="update" accept=".bin" required>
            <button type="submit">Upload Firmware</button>
        </form>
        <small>Keep the browser open until the upload finishes.</small>
    </div>
</body>
</html>
)rawliteral";

unsigned long lastVitalsTime = 0;
unsigned long lastDataTime = 0;

// Internal helpers
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len);
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void configureOtaRoutes();

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

    // 2. Web Server & File System Mount
    if (!LittleFS.begin())
    {
        webLog(0, LOG_ERR, "LittleFS Mount Failed!");
    }
    else
    {
        server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    }

    // 3. Native OTA Mount
    configureOtaRoutes();

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

    // 1-second cadence for Vitals
    if (currentMillis - lastVitalsTime >= 1000)
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
}

void configureOtaRoutes()
{
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request)
              { request->send_P(200, "text/html", OTA_PAGE); });

    server.on("/ota/upload", HTTP_POST, [](AsyncWebServerRequest *request)
              {
                  const bool success = !Update.hasError();
                  AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", success ? "OK" : "FAIL");
                  response->addHeader("Connection", "close");
                  request->send(response);

                  if (success)
                  {
                      webLog(0, LOG_INFO, "OTA Update Complete. Rebooting...");
                      delay(500);
                      ESP.restart();
                  }
                  else
                  {
                      webLog(0, LOG_ERR, "OTA Update Failed");
                  } }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
              {
                  (void)request;
                  (void)filename;

                  if (index == 0)
                  {
                      webLog(0, LOG_INFO, "OTA Update Started");
                      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH))
                      {
                          Update.printError(Serial);
                      }
                  }

                  if (len > 0 && Update.write(data, len) != len)
                  {
                      Update.printError(Serial);
                  }

                  if (final && !Update.end(true))
                  {
                      Update.printError(Serial);
                  } });
}

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
    doc["firebase_ready"] = (WiFi.status() == WL_CONNECTED && String(currentConfig.fb_api_key).length() > 0);

    String payload;
    serializeJson(doc, payload);
    ws.textAll(payload);
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

    // Send pins to JS UI for settings rendering
    // Order MUST match JS parser: TDS, DHT, pH, WaterTemp, WaterLevel, SDA, SCL
    JsonArray pins = doc["pins"].to<JsonArray>();
    pins.add(currentConfig.pin_tds);
    pins.add(currentConfig.pin_dht);
    pins.add(currentConfig.pin_ph);
    pins.add(currentConfig.pin_ds18b20);
    pins.add(currentConfig.pin_wl);
    pins.add(currentConfig.pin_lux_sda);
    pins.add(currentConfig.pin_lux_scl);

    String payload;
    serializeJson(doc, payload);
    ws.textAll(payload);
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

    String payload;
    serializeJson(doc, payload);
    ws.textAll(payload);
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
    if (type == WS_EVT_CONNECT)
    {
        webLog(0, LOG_INFO, "WS Client Connected: " + String(client->id()));
        broadcastConfig(); // Sync UI immediately
    }
    else if (type == WS_EVT_DISCONNECT)
    {
        webLog(0, LOG_INFO, "WS Client Disconnected: " + String(client->id()));
    }
    else if (type == WS_EVT_DATA)
    {
        handleWebSocketMessage(arg, data, len);
    }
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len)
{
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
    {
        data[len] = 0;
        String msg = (char *)data;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, msg);

        if (err)
        {
            webLog(0, LOG_ERR, "WS Parse Error: " + String(err.c_str()));
            return;
        }

        String cmd = doc["command"].as<String>();

        if (cmd == "save_wifi")
        {
            strlcpy(currentConfig.wifi_ssid, doc["ssid"] | "", sizeof(currentConfig.wifi_ssid));
            strlcpy(currentConfig.wifi_pass, doc["pass"] | "", sizeof(currentConfig.wifi_pass));
            state_save();
            broadcastConfig();
            webLog(0, LOG_INFO, "WiFi config saved. Reboot to apply.");
        }
        else if (cmd == "save_firebase")
        {
            strlcpy(currentConfig.fb_api_key, doc["api"] | "", sizeof(currentConfig.fb_api_key));
            strlcpy(currentConfig.fb_project, doc["proj"] | "", sizeof(currentConfig.fb_project));
            strlcpy(currentConfig.fb_email, doc["email"] | "", sizeof(currentConfig.fb_email));
            strlcpy(currentConfig.fb_pass, doc["pass"] | "", sizeof(currentConfig.fb_pass));
            strlcpy(currentConfig.fb_collection, doc["col"] | "", sizeof(currentConfig.fb_collection));
            state_save();
            broadcastConfig();
            webLog(0, LOG_INFO, "Firebase config updated.");
        }
        else if (cmd == "save_pins")
        {
            currentConfig.pin_tds = doc["pin_tds"] | currentConfig.pin_tds;
            currentConfig.pin_dht = doc["pin_dht"] | currentConfig.pin_dht;
            currentConfig.pin_ph = doc["pin_ph"] | currentConfig.pin_ph;
            currentConfig.pin_ds18b20 = doc["pin_wt"] | currentConfig.pin_ds18b20;
            currentConfig.pin_wl = doc["pin_wl"] | currentConfig.pin_wl;
            currentConfig.pin_lux_sda = doc["pin_sda"] | currentConfig.pin_lux_sda;
            currentConfig.pin_lux_scl = doc["pin_scl"] | currentConfig.pin_lux_scl;
            state_save();
            broadcastConfig();
            webLog(0, LOG_INFO, "Pinout config saved. Reboot required.");
        }
        else if (cmd == "calibrate_ph")
        {
            currentConfig.ph_offset = doc["offset"] | currentConfig.ph_offset;
            currentConfig.ph_slope = doc["slope"] | currentConfig.ph_slope;
            state_save();
            broadcastConfig();
            webLog(0, LOG_INFO, "pH Calibration saved.");
        }
        else if (cmd == "calibrate_tds")
        {
            currentConfig.tds_k = doc["tds_k"] | currentConfig.tds_k;
            state_save();
            broadcastConfig();
            webLog(0, LOG_INFO, "TDS Calibration saved.");
        }
        else if (cmd == "factory_reset")
        {
            webLog(0, LOG_WARN, "Factory reset initiated. Wiping NVS...");
            state_factory_reset(); // This handles clear, end, and restart natively.
        }
        else if (cmd == "reboot")
        {
            webLog(0, LOG_WARN, "Manual reboot requested...");
            delay(1000);
            ESP.restart();
        }
        else if (cmd == "request_vitals")
        {
            broadcastVitals();
        }
    }
}
