#include "state.h"
#include <Preferences.h>
#include <LittleFS.h>

// ---------- Globals ----------
ConfigState  currentConfig;
SensorState  currentSensors;
VitalsState  currentVitals;

// Forward decl — actual array lives in src/sensors/sensors.cpp
// extern const bool sensor_impl[S_COUNT];

static Preferences prefs;
static const char* NVS_NS = "hygrow";

// webLog is implemented in task_network.cpp (needs access to the AsyncWebSocket).
// We only provide the back-compat overload here.
extern void webLog(uint8_t core, uint8_t level, const String& msg);
void webLog(const String& msg) { webLog(0, LOG_INFO, msg); }

// ---------- Helpers ----------
static void loadStr(const char* key, char* dst, size_t dstSize, const char* def) {
  String v = prefs.getString(key, def);
  strncpy(dst, v.c_str(), dstSize - 1);
  dst[dstSize - 1] = '\0';
}

// ---------- Init ----------
void state_init() {
  // LittleFS (web assets)
  currentVitals.littlefs_ok = LittleFS.begin(false);
  if (!currentVitals.littlefs_ok) {
    // try format-on-fail once
    currentVitals.littlefs_ok = LittleFS.begin(true);
  }

  // NVS
  prefs.begin(NVS_NS, false);

  // WiFi
  loadStr("wifi_ssid", currentConfig.wifi_ssid, sizeof(currentConfig.wifi_ssid), DEFAULT_WIFI_SSID);
  loadStr("wifi_pass", currentConfig.wifi_pass, sizeof(currentConfig.wifi_pass), DEFAULT_WIFI_PASS);
  loadStr("ap_pass",   currentConfig.ap_pass,   sizeof(currentConfig.ap_pass),   DEFAULT_AP_PASS);

  // Firebase
  loadStr("fb_api",   currentConfig.fb_api_key,   sizeof(currentConfig.fb_api_key),   DEFAULT_FB_API_KEY);
  loadStr("fb_proj",  currentConfig.fb_project,   sizeof(currentConfig.fb_project),   DEFAULT_FB_PROJECT);
  loadStr("fb_email", currentConfig.fb_email,     sizeof(currentConfig.fb_email),     DEFAULT_FB_EMAIL);
  loadStr("fb_pass",  currentConfig.fb_pass,      sizeof(currentConfig.fb_pass),      DEFAULT_FB_PASS);
  loadStr("fb_col",   currentConfig.fb_collection,sizeof(currentConfig.fb_collection),DEFAULT_FB_COLLECTION);
  loadStr("dev_id",   currentConfig.device_id,    sizeof(currentConfig.device_id),    DEFAULT_DEVICE_ID);

  // Timing
  currentConfig.interval_read_ms = prefs.getUInt("int_read", DEFAULT_INTERVAL_READ_MS);
  currentConfig.interval_ws_ms   = prefs.getUInt("int_ws",   DEFAULT_INTERVAL_WS_MS);
  currentConfig.interval_fb_ms   = prefs.getUInt("int_fb",   DEFAULT_INTERVAL_FB_MS);

  // Calibration
  currentConfig.ph_offset = prefs.getFloat("ph_off",   DEFAULT_PH_OFFSET);
  currentConfig.ph_slope  = prefs.getFloat("ph_slope", DEFAULT_PH_SLOPE);
  currentConfig.tds_k     = prefs.getFloat("tds_k",    DEFAULT_TDS_K);

  // Pins (Changed to getInt to support -1 disabled flag)
  currentConfig.pin_dht     = prefs.getInt("pin_dht",     PIN_DHT);
  currentConfig.pin_ds18b20 = prefs.getInt("pin_ds",      PIN_DS18B20);
  currentConfig.pin_tds     = prefs.getInt("pin_tds",     PIN_TDS);
  currentConfig.pin_ph      = prefs.getInt("pin_ph",      PIN_PH);
  currentConfig.pin_lux_sda = prefs.getInt("pin_sda",     PIN_LUX_SDA);
  currentConfig.pin_lux_scl = prefs.getInt("pin_scl",     PIN_LUX_SCL);
  currentConfig.pin_wl      = prefs.getInt("pin_wl",      PIN_WL);

  // Feature flags — one NVS key per sensor: "en_0" .. "en_N"
  for (int i = 0; i < S_COUNT; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "en_%d", i);
    currentConfig.sensor_enabled[i] = prefs.getBool(k, true);
  }

  // Init telemetry
  memset(&currentSensors, 0, sizeof(currentSensors));
  memset(&currentVitals,  0, sizeof(currentVitals));
}

// ---------- Save ----------
bool state_save() {
  prefs.putString("wifi_ssid", currentConfig.wifi_ssid);
  prefs.putString("wifi_pass", currentConfig.wifi_pass);
  prefs.putString("ap_pass",   currentConfig.ap_pass);

  prefs.putString("fb_api",   currentConfig.fb_api_key);
  prefs.putString("fb_proj",  currentConfig.fb_project);
  prefs.putString("fb_email", currentConfig.fb_email);
  prefs.putString("fb_pass",  currentConfig.fb_pass);
  prefs.putString("fb_col",   currentConfig.fb_collection);
  prefs.putString("dev_id",   currentConfig.device_id);

  prefs.putUInt("int_read", currentConfig.interval_read_ms);
  prefs.putUInt("int_ws",   currentConfig.interval_ws_ms);
  prefs.putUInt("int_fb",   currentConfig.interval_fb_ms);

  prefs.putFloat("ph_off",   currentConfig.ph_offset);
  prefs.putFloat("ph_slope", currentConfig.ph_slope);
  prefs.putFloat("tds_k",    currentConfig.tds_k);

  // Pins (Changed to putInt to support -1 disabled flag)
  prefs.putInt("pin_dht", currentConfig.pin_dht);
  prefs.putInt("pin_ds",  currentConfig.pin_ds18b20);
  prefs.putInt("pin_tds", currentConfig.pin_tds);
  prefs.putInt("pin_ph",  currentConfig.pin_ph);
  prefs.putInt("pin_sda", currentConfig.pin_lux_sda);
  prefs.putInt("pin_scl", currentConfig.pin_lux_scl);
  prefs.putInt("pin_wl",  currentConfig.pin_wl);

  for (int i = 0; i < S_COUNT; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "en_%d", i);
    prefs.putBool(k, currentConfig.sensor_enabled[i]);
  }
  return true;
}

// ---------- Factory reset ----------
void state_factory_reset() {
  prefs.clear();
  prefs.end();
  delay(200);
  ESP.restart();
}
