#ifndef STATE_H
#define STATE_H

#include <Arduino.h>
#include "../../config.h"

// Forward declaration only — avoids pulling ESPAsyncWebServer.h (and its
// AsyncTCP dependency) into every sensor_*.cpp that includes state.h just
// for webLogSendBacklog()'s pointer parameter below. The real definition
// lives in task_network.cpp/.h, which is where state.cpp gets it from.
class AsyncWebSocketClient;

// ---------- Runtime config (mirrors NVS) ----------
struct ConfigState
{
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
  uint32_t interval_vitals_ms; // vitals push period
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
  int pin_wl_power;

  // Feature flags — user-editable from Web Doctor > Settings > Feature Flags
  bool demo_mode;         // [NVS] demo    — simulate sensor data instead of reading hardware
  bool firebase_enabled;  // [NVS] fb_en   — gate the Firestore POST logic

  // Feature flags — user toggles per sensor
  bool sensor_enabled[S_COUNT];
};

// ---------- Live telemetry (latest read) ----------
struct SensorState
{
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
struct VitalsState
{
  int32_t rssi;
  uint32_t heap_free;
  uint32_t heap_min_free;
  uint32_t uptime_s;
  bool wifi_connected;
  bool ap_active;
  char ip[16];
  char ap_ip[16];
  bool firebase_ready;      // reflects the outcome of the most recent real upload attempt
  uint32_t firebase_last_ok_ms;   // millis() of last successful Firestore upload; 0 = never
  char firebase_last_error[64];   // last upload error string; "" = no error recorded
  bool littlefs_ok;
};

// ---------- Globals (defined in state.cpp) ----------
extern ConfigState currentConfig;
extern SensorState currentSensors;
extern VitalsState currentVitals;

// Sensor helper forward declarations — task_sensor.cpp is the single
// source of truth for hardware I/O and is the only caller of these.
// Each sensor_*_init() returns true iff the sensor is ready to be read
// (pin(s) assigned and, where the hardware supports it, actually detected
// on the bus at boot); sensor_*_read() returns true iff the read succeeded.
void sensor_dht_init();
bool sensor_dht_read(float &temp_c, float &humidity_pct);
void sensor_ds18b20_init();
bool sensor_ds18b20_read(float &temp_c);
void sensor_tds_init();
bool sensor_tds_read(float water_temp_c, float tds_k, float &tds_ppm);
void sensor_ph_init();
bool sensor_ph_read(float ph_offset, float ph_slope, float &ph_value);
bool sensor_lux_init(); // returns true only if a BH1750 actually ACKed on I2C
bool sensor_lux_read(float &lux);
void sensor_wl_init();
bool sensor_wl_read(float &percent);

// ---------- API ----------
void state_init();          // mount NVS, load config (defaults from config.h if unset)
bool state_save();          // persist currentConfig to NVS
void state_factory_reset(); // wipe NVS + reboot

// ---------- Single-owner auth (admin password + session token) ----------
// Deliberately its own tiny NVS namespace/lifecycle, NOT part of ConfigState/
// state_save()/state_init() above. Two reasons:
//   1. Every ordinary settings save (pins, Wi-Fi, calibration, ...) calls
//      state_save() — if the password lived in ConfigState, every one of
//      those saves would also rewrite the password blob for no reason.
//   2. The BOOT-button "10s hold" reset (task_network.cpp) wipes ONLY the
//      admin password/token and explicitly must leave Wi-Fi, sensors, and
//      calibration untouched. That's only possible if auth has its own
//      NVS namespace, independent of the "hygrow" namespace state_save()
//      writes to.
// There is exactly one account ("admin", hardcoded, never stored) — see
// the Login/Set Password overlay in data/js/app.js for the client side.
void auth_init();                          // mount the auth NVS namespace, load state into RAM
bool auth_is_configured();                 // true once an admin password has been set
bool auth_check_password(const String &candidate);
void auth_set_password(const String &newPass); // first-time setup OR admin-initiated change; also issues a fresh session token
String auth_issue_token();                 // generates + persists a new random session token, returns it
bool auth_check_token(const String &candidate);
void auth_reset();                         // wipe ONLY the password + token (BOOT button 10s hold)

// Log helper — core is 0 (network) or 1 (sensor); level is LOG_INFO/WARN/ERR.
// Every call does three things, always in this order, so the Serial monitor
// and the web Terminal (data/index.html Page 9) never drift apart:
//   1. Serial.println() — unconditional, exactly as before.
//   2. Appends to a small in-RAM ring buffer (see webLogSendBacklog() below),
//      so a browser tab that (re)connects late still sees recent history
//      instead of only whatever is logged AFTER it happens to be looking.
//   3. Broadcasts a {"type":"log","core":...,"level":...,"msg":...} WS frame
//      to already-authenticated clients (task_network.cpp defines ws and
//      the auth gate) — this is a no-op if the network task hasn't started
//      yet or no client is authenticated, both fine since Serial already
//      has the message.
// Levels never reach either output as bare numbers: state.cpp maps
// LOG_INFO/WARN/ERR to "info"/"warn"/"error" for both Serial (as a
// bracketed tag) and the WS frame, so the two views print identical text.
void webLog(uint8_t core, uint8_t level, const String &msg);
// Back-compat single-arg form (defaults core=0, level=LOG_INFO)
void webLog(const String &msg);

// Replays the ring buffer of recent log lines to one client, in the order
// they were originally logged. Called once, right after a client passes
// auth (task_network.cpp) — that's the moment "sync" actually matters: a
// browser opened after boot still gets to see what already happened
// (Wi-Fi connect attempts, sensor init results, etc.), not just a blank
// Terminal that only starts filling in from the moment it happened to log in.
void webLogSendBacklog(AsyncWebSocketClient *client);

#endif // STATE_H
