#ifndef STATE_H
#define STATE_H

#include <Arduino.h>
#include "../../config.h"

// Forward declaration only — avoids pulling ESPAsyncWebServer.h (and its
// AsyncTCP dependency) into every sensor_*.cpp that includes state.h just
// for webLogSendBacklog()'s pointer parameter below. The real definition
// lives in ESPAsyncWebServer.h, pulled in by task_network.h/task_network_internal.h,
// which is where state.cpp gets it from.
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

  // Pins (reboot required to apply). Plain GPIO numbers only — a pin is
  // never used as an on/off switch. The single on/off switch for a sensor
  // is sensor_enabled[] below; a pin keeps its saved value even while its
  // sensor is disabled.
  //
  // While demo_mode is true, every one of these fields reads DEMO_MODE_PIN
  // (config.h) instead of a real GPIO — see sensorPinIsDemo() in
  // task_sensor.cpp. The user's real assignments are preserved separately in
  // real_pin_* below so turning Demo Mode back off restores exactly what was
  // there before, not just the compiled defaults.
  int pin_dht;
  int pin_ds18b20;
  int pin_tds;
  int pin_ph;
  int pin_lux_sda;
  int pin_lux_scl;
  int pin_wl;
  int pin_wl_power;

  // Real (non-demo) pin assignments, preserved while demo_mode is true so
  // save_features (command_handlers.cpp) can restore them verbatim when demo
  // mode is turned back off, instead of falling back to compiled defaults
  // and silently discarding a custom pinout the user saved before enabling
  // Demo Mode. Meaningless/unused while demo_mode is false — pin_* above is
  // the live value in that case and these just mirror it on every save_pins.
  int real_pin_dht;
  int real_pin_ds18b20;
  int real_pin_tds;
  int real_pin_ph;
  int real_pin_lux_sda;
  int real_pin_lux_scl;
  int real_pin_wl;
  int real_pin_wl_power;

  // Feature flags — user-editable from Web Doctor > Settings > Feature Flags
  bool demo_mode;         // [NVS] demo    — simulate sensor data; also swaps every sensor pin to DEMO_MODE_PIN (reboot required, see save_features)
  bool firebase_enabled;  // [NVS] fb_en   — gate the Firestore POST logic

  // Feature flags — the SINGLE on/off switch per sensor. This is checked
  // everywhere a sensor's init/read/upload decision is made; pin_* above is
  // never consulted for that decision.
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

  // True whenever the most recent real TDS reading (S_TDS enabled, not
  // itself in demo mode) was temperature-compensated against a neutral
  // 25.0°C placeholder instead of a real water_temp_c reading, because
  // Water Temp (S_WTEMP) was independently in demo mode at read time — see
  // the readAll() TDS block in task_sensor.cpp. Lets the frontend show a
  // "using placeholder water temp" note on the TDS card instead of the
  // reading silently looking fully live. Meaningless/stale whenever TDS
  // itself is disabled or in demo mode (readAllDemo()'s simulated TDS
  // never touches this flag either way, so it just holds whatever it was
  // last set to — the frontend only surfaces it while dash-dot-tds shows
  // "live", i.e. TDS is enabled and not itself simulated).
  bool tds_comp_using_fake_water_temp;

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
// Persists currentConfig to NVS and returns true only if every field
// actually made it to flash. Preferences::putX() returns the number of
// bytes written (0 on failure, e.g. a full/worn/corrupted NVS partition),
// so state_save() checks each call instead of assuming success — callers
// (the command handlers in src/core/command_handlers.cpp) surface a real
// "Failed to save" to the client instead of an unconditional ack.
bool state_save();
void state_factory_reset(); // wipe NVS + reboot

// ---------- Crash / reboot diagnostics ----------
// Persists WHY the device restarted (esp_reset_reason(), captured in the
// .ino's setup()) into its own tiny NVS namespace, separate from both
// NVS_NS (ordinary config) and AUTH_NVS_NS (password/token) — a crash
// should never be lost alongside, or by, either of those. Call
// state_log_reset_reason() once, early in setup(), with a human-readable
// reason string; state_init() then makes the PREVIOUS boot's reason
// available via state_get_last_reset_reason() before this call overwrites
// it with the current one. This is what lets a browser opened well after a
// crash still see (via the Terminal's log backlog) why the board came back
// up, instead of that information only ever existing on a Serial monitor
// that happened to be attached at the exact moment it happened.
void state_log_reset_reason(const char *reason);
// Returns the reset reason string recorded on the PREVIOUS boot (before
// state_log_reset_reason() is called for the current one), or "" if none
// was ever recorded (e.g. very first boot).
String state_get_last_reset_reason();

// ---------- Single-owner auth (admin password + session token) ----------
// Deliberately its own tiny NVS namespace/lifecycle, NOT part of ConfigState/
// state_save()/state_init() above. Two reasons:
//   1. Every ordinary settings save (pins, Wi-Fi, calibration, ...) calls
//      state_save() — if the password lived in ConfigState, every one of
//      those saves would also rewrite the password blob for no reason.
//   2. The BOOT-button "10s hold" reset (HyGrow_IoT.ino) wipes ONLY the
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
// Boot-time Serial banner only (HyGrow_IoT.ino setup()) — never sent over
// the network. See the comment on the definition in state.cpp.
String auth_get_password_for_boot_display();
// Returns the current plaintext admin password, or "" if unconfigured.
// Unlike auth_get_password_for_boot_display() above, this IS sent over the
// network — see the comment on its definition in state.cpp for the trust
// tradeoff this represents before calling it from anywhere new.
String auth_get_password_for_ws();

// Log helper — core is 0 (network) or 1 (sensor); level is LOG_INFO/WARN/ERR.
// Every call does three things, always in this order, so the Serial monitor
// and the web Terminal (data/index.html Page 9) never drift apart:
//   1. Serial.println() — unconditional, exactly as before.
//   2. Appends to a small in-RAM ring buffer (see webLogSendBacklog() below),
//      so a browser tab that (re)connects late still sees recent history
//      instead of only whatever is logged AFTER it happens to be looking.
//   3. Broadcasts a {"type":"log","core":...,"level":...,"msg":...} WS frame
//      to already-authenticated clients (`ws` is defined in task_network.cpp,
//      the auth gate in auth.cpp/websocket.cpp) — this is a no-op if the
//      network task hasn't started yet or no client is authenticated, both
//      fine since Serial already has the message.
// Levels never reach either output as bare numbers: state.cpp maps
// LOG_INFO/WARN/ERR to "info"/"warn"/"error" for both Serial (as a
// bracketed tag) and the WS frame, so the two views print identical text.
void webLog(uint8_t core, uint8_t level, const String &msg);
// Back-compat single-arg form (defaults core=0, level=LOG_INFO)
void webLog(const String &msg);

// Serial-only, in-place-updating progress line for retry loops (e.g. sensor
// startup validation) — see the long comment on the definition in state.cpp
// for exactly why this is Serial-only and doesn't touch the ring buffer or
// WebSocket. Call webLogProgress() once per attempt, then webLogProgressDone()
// once before logging the final permanent result via the normal webLog().
void webLogProgress(const String &msg);
void webLogProgressDone();

// Serial-only boxed section header (e.g. "SYSTEM", "NETWORK") — see the long
// comment on the definition in state.cpp for why this bypasses webLog()
// entirely (ring buffer + WebSocket) and is Serial-only.
void printBootSection(const char *title);

// Replays the ring buffer of recent log lines to one client, in the order
// they were originally logged. Called once, right after a client passes
// auth (handleAuthCommand() in auth.cpp) — that's the moment "sync" actually
// matters: a browser opened after boot still gets to see what already
// happened (Wi-Fi connect attempts, sensor init results, etc.), not just a
// blank Terminal that only starts filling in from the moment it happened to
// log in.
void webLogSendBacklog(AsyncWebSocketClient *client);

#endif // STATE_H
