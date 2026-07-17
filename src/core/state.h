#ifndef STATE_H
#define STATE_H

#include <Arduino.h>
#include "../../config.h"

// ---------- Runtime config (mirrors NVS) ----------
struct ConfigState {
  // WiFi
  char wifi_ssid[33];
  char wifi_pass[65];
  char ap_pass[65];

  // Firebase
  char fb_api_key[128];
  char fb_project[64];
  char fb_email[64];
  char fb_pass[64];
  char fb_collection[32];
  char device_id[32];

  // Timing (ms)
  uint32_t interval_read_ms;   // sensor sample period
  uint32_t interval_ws_ms;     // websocket push period
  uint32_t interval_fb_ms;     // firestore push period

  // Calibration
  float ph_offset;
  float ph_slope;
  float tds_k;

  // Pins (reboot required to apply) - changed to int to allow -1 (disabled)
  int pin_dht;
  int pin_ds18b20;
  int pin_tds;
  int pin_ph;
  int pin_lux_sda;
  int pin_lux_scl;
  int pin_wl;

  // Feature flags — user toggles per sensor
  bool sensor_enabled[S_COUNT];
};

// ---------- Live telemetry (latest read) ----------
struct SensorState {
  float temp_c;
  float humidity;
  float water_temp_c;
  float tds_ppm;
  float lux;
  float ph_val;
  float wl_percent;
  float vpd_kpa;

  // Per-sensor last-good timestamp (millis); 0 = never
  uint32_t last_ok_ms[S_COUNT];
  // Per-sensor last error string ("" = ok)
  char last_err[S_COUNT][48];
};

// ---------- Diagnostics ----------
struct VitalsState {
  int32_t  rssi;
  uint32_t heap_free;
  uint32_t heap_min_free;
  uint32_t uptime_s;
  bool     wifi_connected;
  bool     ap_active;
  char     ip[16];
  char     ap_ip[16];
  bool     firebase_ready;
  bool     littlefs_ok;
};

// ---------- Globals (defined in state.cpp) ----------
extern ConfigState  currentConfig;
extern SensorState  currentSensors;
extern VitalsState  currentVitals;

// Reported by each sensor .cpp at compile time; assembled in sensors.cpp
extern const bool sensor_impl[S_COUNT];

// ---------- API ----------
void state_init();                 // mount NVS, load config (defaults from config.h if unset)
bool state_save();                 // persist currentConfig to NVS
void state_factory_reset();        // wipe NVS + reboot

// Log helper — core is 0 (network) or 1 (sensor); level is LOG_INFO/WARN/ERR
void webLog(uint8_t core, uint8_t level, const String& msg);
// Back-compat single-arg form (defaults core=0, level=LOG_INFO)
void webLog(const String& msg);

#endif // STATE_H
