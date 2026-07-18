#include "state.h"
#include <Preferences.h>
#include <LittleFS.h>

// ---------- Globals ----------
ConfigState currentConfig;
SensorState currentSensors;
VitalsState currentVitals;

static Preferences prefs;
static const char *NVS_NS = "hygrow";

// Provide a default logger for both Arduino IDE and PlatformIO builds.
void webLog(uint8_t core, uint8_t level, const String &msg)
{
  (void)core;
  (void)level;
  Serial.println(msg);
}

void webLog(const String &msg) { webLog(0, LOG_INFO, msg); }

// ---------- Helpers ----------
static void loadStr(const char *key, char *dst, size_t dstSize, const char *def)
{
  String v = prefs.getString(key, def);
  strncpy(dst, v.c_str(), dstSize - 1);
  dst[dstSize - 1] = '\0';
}

// Same [500, 60000]ms bounds save_intervals already enforces before writing
// to NVS (task_network.cpp) — reapplied here at load time too, so a value
// that predates that clamp (an older NVS blob) or got corrupted on the wire
// can't leave e.g. interval_read_ms at 0. A 0ms read interval would make
// sensorTaskLoop()'s "time since last read >= interval" check always true,
// spinning the sensor task in a tight loop — continuously re-triggering the
// TDS sensor's ~60ms blocking 30-sample read and the water-level sensor's
// power-gate cycling back to back, instead of the intended periodic cadence.
static uint32_t clampInterval(uint32_t ms)
{
  if (ms < 500)
    return 500;
  if (ms > 60000)
    return 60000;
  return ms;
}

// ---------- Init ----------
void state_init()
{
  // LittleFS (web assets)
  // NOTE: Do NOT auto-format on a failed mount here. Silently retrying with
  // LittleFS.begin(true) used to mask a failed/empty mount by reporting
  // littlefs_ok = true even though the web asset partition was just wiped.
  // We now report the real mount result and let the halt gate in the .ino
  // (setup()) catch a bad mount and stop, instead of continuing silently
  // into a broken state.
  currentVitals.littlefs_ok = LittleFS.begin(false);

  // NVS
  prefs.begin(NVS_NS, false);

  // WiFi
  loadStr("wifi_ssid", currentConfig.wifi_ssid, sizeof(currentConfig.wifi_ssid), DEFAULT_WIFI_SSID);
  loadStr("wifi_pass", currentConfig.wifi_pass, sizeof(currentConfig.wifi_pass), DEFAULT_WIFI_PASS);
  loadStr("ap_pass", currentConfig.ap_pass, sizeof(currentConfig.ap_pass), DEFAULT_AP_PASS);

  // Firebase
  loadStr("fb_api", currentConfig.fb_api_key, sizeof(currentConfig.fb_api_key), DEFAULT_FB_API_KEY);
  loadStr("fb_proj", currentConfig.fb_project, sizeof(currentConfig.fb_project), DEFAULT_FB_PROJECT);
  loadStr("fb_email", currentConfig.fb_email, sizeof(currentConfig.fb_email), DEFAULT_FB_EMAIL);
  loadStr("fb_pass", currentConfig.fb_pass, sizeof(currentConfig.fb_pass), DEFAULT_FB_PASS);
  loadStr("fb_col", currentConfig.fb_collection, sizeof(currentConfig.fb_collection), DEFAULT_FB_COLLECTION);
  loadStr("dev_id", currentConfig.device_id, sizeof(currentConfig.device_id), DEFAULT_DEVICE_ID);

  // Timing. Clamped the same as save_intervals (task_network.cpp) so an old
  // pre-clamp NVS value or corrupted entry can't leave an interval at an
  // unsafe extreme (see clampInterval()'s comment above for why 0 is
  // dangerous specifically).
  currentConfig.interval_read_ms = clampInterval(prefs.getUInt("int_read", DEFAULT_INTERVAL_READ_MS));
  currentConfig.interval_ws_ms = clampInterval(prefs.getUInt("int_ws", DEFAULT_INTERVAL_WS_MS));
  currentConfig.interval_vitals_ms = clampInterval(prefs.getUInt("int_vit", DEFAULT_INTERVAL_VITALS_MS));
  currentConfig.interval_fb_ms = clampInterval(prefs.getUInt("int_fb", DEFAULT_INTERVAL_FB_MS));

  // Calibration
  currentConfig.ph_offset = prefs.getFloat("ph_off", DEFAULT_PH_OFFSET);
  currentConfig.ph_slope = prefs.getFloat("ph_slope", DEFAULT_PH_SLOPE);
  currentConfig.tds_k = prefs.getFloat("tds_k", DEFAULT_TDS_K);

  // Pins (Changed to getInt to support -1 disabled flag)
  currentConfig.pin_dht = prefs.getInt("pin_dht", PIN_DHT);
  currentConfig.pin_ds18b20 = prefs.getInt("pin_ds", PIN_DS18B20);
  currentConfig.pin_tds = prefs.getInt("pin_tds", PIN_TDS);
  currentConfig.pin_ph = prefs.getInt("pin_ph", PIN_PH);
  currentConfig.pin_lux_sda = prefs.getInt("pin_sda", PIN_LUX_SDA);
  currentConfig.pin_lux_scl = prefs.getInt("pin_scl", PIN_LUX_SCL);
  currentConfig.pin_wl = prefs.getInt("pin_wl", PIN_WL);
  currentConfig.pin_wl_power = prefs.getInt("pin_wlp", PIN_WL_PWR);

  // Feature flags — demo mode / Firebase upload gate / OTA gate.
  // NVS key strings match the [NVS] comments next to each #define in
  // config.h (demo, fb_en, ota_en), so the code stays self-documenting.
  currentConfig.demo_mode        = prefs.getBool("demo",   DEFAULT_DEMO_MODE);
  currentConfig.firebase_enabled = prefs.getBool("fb_en",  DEFAULT_FIREBASE_ENABLED);
  currentConfig.ota_enabled      = prefs.getBool("ota_en", DEFAULT_OTA_ENABLED);

  // Feature flags — one NVS key per sensor: "en_0" .. "en_N". Every sensor
  // defaults to DEFAULT_SENSOR_ENABLED (true) except pH (S_PH), which uses
  // its own DEFAULT_PH_SENSOR_ENABLED (false) — see config.h for why. This
  // only matters on first boot / after a factory reset; once a value is
  // saved to NVS, that saved value always wins over either default.
  for (int i = 0; i < S_COUNT; ++i)
  {
    char k[8];
    snprintf(k, sizeof(k), "en_%d", i);
    bool defaultForThisSensor = (i == S_PH) ? DEFAULT_PH_SENSOR_ENABLED : DEFAULT_SENSOR_ENABLED;
    currentConfig.sensor_enabled[i] = prefs.getBool(k, defaultForThisSensor);
  }

  // Init telemetry
  memset(&currentSensors, 0, sizeof(currentSensors));
  memset(&currentVitals, 0, sizeof(currentVitals));
}

// ---------- Save ----------
bool state_save()
{
  prefs.putString("wifi_ssid", currentConfig.wifi_ssid);
  prefs.putString("wifi_pass", currentConfig.wifi_pass);
  prefs.putString("ap_pass", currentConfig.ap_pass);

  prefs.putString("fb_api", currentConfig.fb_api_key);
  prefs.putString("fb_proj", currentConfig.fb_project);
  prefs.putString("fb_email", currentConfig.fb_email);
  prefs.putString("fb_pass", currentConfig.fb_pass);
  prefs.putString("fb_col", currentConfig.fb_collection);
  prefs.putString("dev_id", currentConfig.device_id);

  prefs.putUInt("int_read", currentConfig.interval_read_ms);
  prefs.putUInt("int_ws", currentConfig.interval_ws_ms);
  prefs.putUInt("int_vit", currentConfig.interval_vitals_ms);
  prefs.putUInt("int_fb", currentConfig.interval_fb_ms);

  prefs.putFloat("ph_off", currentConfig.ph_offset);
  prefs.putFloat("ph_slope", currentConfig.ph_slope);
  prefs.putFloat("tds_k", currentConfig.tds_k);

  // Pins (Changed to putInt to support -1 disabled flag)
  prefs.putInt("pin_dht", currentConfig.pin_dht);
  prefs.putInt("pin_ds", currentConfig.pin_ds18b20);
  prefs.putInt("pin_tds", currentConfig.pin_tds);
  prefs.putInt("pin_ph", currentConfig.pin_ph);
  prefs.putInt("pin_sda", currentConfig.pin_lux_sda);
  prefs.putInt("pin_scl", currentConfig.pin_lux_scl);
  prefs.putInt("pin_wl", currentConfig.pin_wl);
  prefs.putInt("pin_wlp", currentConfig.pin_wl_power);

  for (int i = 0; i < S_COUNT; ++i)
  {
    char k[8];
    snprintf(k, sizeof(k), "en_%d", i);
    prefs.putBool(k, currentConfig.sensor_enabled[i]);
  }

  prefs.putBool("demo",   currentConfig.demo_mode);
  prefs.putBool("fb_en",  currentConfig.firebase_enabled);
  prefs.putBool("ota_en", currentConfig.ota_enabled);

  return true;
}

// ---------- Factory reset ----------
void state_factory_reset()
{
  prefs.clear();
  prefs.end();
  delay(200);
  ESP.restart();
}
