#include "state.h"
#include "task_network.h" // wsBroadcastLog() — see the comment on webLog() below for why
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_partition.h> // esp_partition_find_first() — [FS DIAG] block in state_init()

// ---------- Globals ----------
ConfigState currentConfig;
SensorState currentSensors;
VitalsState currentVitals;

static Preferences prefs;
static const char *NVS_NS = "hygrow";

// Single-owner auth state — separate Preferences namespace from NVS_NS above
// (see the long comment on the auth_*() declarations in state.h for why).
static Preferences authPrefs;
static const char *AUTH_NVS_NS = "hygrow_auth";
static char s_adminPass[65] = {0};  // "" == unconfigured (no password set yet)
static char s_sessionToken[33] = {0}; // "" == no valid session token issued

// Crash/reboot diagnostics — its own tiny NVS namespace, independent of
// both NVS_NS (ordinary config, wiped on factory reset) and AUTH_NVS_NS
// (password/token, wiped on either reset type). A crash reason is exactly
// the kind of thing you still want to see AFTER a factory reset wipes
// everything else, so it deliberately lives outside that blast radius.
static Preferences crashPrefs;
static const char *CRASH_NVS_NS = "hygrow_crash";
static char s_lastResetReason[64] = {0}; // "" == none recorded yet (first boot)

// ----------------------------------------------------------------------------
// webLog() — single source of truth for BOTH the Serial monitor and the web
// Terminal (data/index.html Page 9). Every call site across the firmware
// (sensor drivers, task_sensor.cpp, the split task_network.cpp/auth.cpp/
// firebase.cpp/websocket.cpp/command_handlers.cpp files, HyGrow_IoT.ino)
// already goes through this one function, so fixing it here is enough to
// make the two outputs match everywhere at once — no call sites need to
// change.
// ----------------------------------------------------------------------------

// Small ring buffer of recent log lines, kept purely in RAM (never persisted
// to NVS/flash — restart wipes it, same as the Serial monitor's own
// scrollback would be lost on a fresh terminal connection). Sized generously
// enough to cover a full boot sequence (Wi-Fi connect/AP fallback + every
// sensor's init line) plus normal runtime chatter, at a fixed, small memory
// cost: LOG_BACKLOG_CAPACITY * sizeof(LogEntry) is a few KB, trivial on the
// ESP32-S3's RAM budget.
#define LOG_BACKLOG_CAPACITY 40
#define LOG_MSG_MAX 128

struct LogEntry
{
  uint8_t core;
  uint8_t level;
  char msg[LOG_MSG_MAX];
};

static LogEntry s_logBacklog[LOG_BACKLOG_CAPACITY];
static uint16_t s_logCount = 0;    // number of valid entries, caps at CAPACITY
static uint16_t s_logNext = 0;     // next write slot (wraps)

// Maps LOG_INFO/LOG_WARN/LOG_ERR (config.h) to the exact strings the
// frontend's updateTerminal() (data/js/app.js) already switches on for
// styling ("error" -> bold red, "warn" -> secondary color). Used for BOTH
// the WS frame's "level" field and the bracketed Serial tag below, so a
// line reads identically ("[WARN] ...") whether you're watching the Serial
// monitor or the web Terminal.
static const char *levelToTag(uint8_t level)
{
  switch (level)
  {
    case LOG_WARN: return "WARN";
    case LOG_ERR:  return "ERROR";
    default:       return "INFO";
  }
}

static const char *levelToJsonString(uint8_t level)
{
  switch (level)
  {
    case LOG_WARN: return "warn";
    case LOG_ERR:  return "error";
    default:       return "info";
  }
}

void webLog(uint8_t core, uint8_t level, const String &msg)
{
  // 1. Serial — unconditional, tagged with the same level word the web
  // Terminal shows, and the core number so a mixed sensor/network boot log
  // reads the same way in both places: "[CORE 1] [INFO] DHT22 initialized...".
  Serial.println("[CORE " + String(core) + "] [" + String(levelToTag(level)) + "] " + msg);

  // 2. Ring buffer — record this line before broadcasting, so a client that
  // authenticates in the middle of a burst of logs (e.g. during boot) still
  // gets it via the backlog replay even if it narrowly missed the live frame.
  LogEntry &slot = s_logBacklog[s_logNext];
  slot.core = core;
  slot.level = level;
  strncpy(slot.msg, msg.c_str(), LOG_MSG_MAX - 1);
  slot.msg[LOG_MSG_MAX - 1] = '\0';
  s_logNext = (s_logNext + 1) % LOG_BACKLOG_CAPACITY;
  if (s_logCount < LOG_BACKLOG_CAPACITY) s_logCount++;

  // 3. WS broadcast — same shape data/js/app.js's updateTerminal() already
  // parses (see the msg.type === "log" dispatch in app.js). wsBroadcastLog()
  // silently no-ops if the network task hasn't started yet or no client is
  // authenticated yet; Serial (step 1) already has the message regardless.
  JsonDocument doc;
  doc["type"] = "log";
  doc["core"] = core;
  doc["level"] = levelToJsonString(level);
  doc["msg"] = msg;
  String payload;
  serializeJson(doc, payload);
  wsBroadcastLog(payload);
}

void webLog(const String &msg) { webLog(0, LOG_INFO, msg); }

// Replays the ring buffer to one newly-authenticated client, oldest first,
// so its Terminal reads top-to-bottom the same way the Serial monitor's
// scrollback would if you'd been connected since boot. Called from
// handleAuthCommand() in auth.cpp right after a client passes auth.
void webLogSendBacklog(AsyncWebSocketClient *client)
{
  if (!client || s_logCount == 0) return;

  // s_logNext is the next WRITE slot, i.e. one past the newest entry. The
  // oldest valid entry is s_logCount behind that (wrapping), whether or not
  // the buffer has filled and started overwriting old entries yet.
  uint16_t start = (s_logNext + LOG_BACKLOG_CAPACITY - s_logCount) % LOG_BACKLOG_CAPACITY;

  for (uint16_t i = 0; i < s_logCount; i++)
  {
    LogEntry &e = s_logBacklog[(start + i) % LOG_BACKLOG_CAPACITY];
    JsonDocument doc;
    doc["type"] = "log";
    doc["core"] = e.core;
    doc["level"] = levelToJsonString(e.level);
    doc["msg"] = e.msg;
    String payload;
    serializeJson(doc, payload);
    client->text(payload);
  }
}

// ---------- Helpers ----------
static void loadStr(Preferences &store, const char *key, char *dst, size_t dstSize, const char *def)
{
  String v = store.getString(key, def);
  strncpy(dst, v.c_str(), dstSize - 1);
  dst[dstSize - 1] = '\0';
}

// Same [500, 60000]ms bounds save_intervals already enforces before writing
// to NVS (command_handlers.cpp) — reapplied here at load time too, so a value
// that predates that clamp (an older NVS blob) or got corrupted on the wire
// can't leave e.g. interval_read_ms at 0. A 0ms read interval would make
// sensorTaskLoop()'s "time since last read >= interval" check always true,
// spinning the sensor task in a tight loop — continuously re-triggering the
// TDS sensor's ~60ms blocking 30-sample read and the water-level sensor's
// power-gate cycling back to back, instead of the intended periodic cadence.
static uint32_t clampInterval(uint32_t ms)
{
  if (ms < 2000)
    return 2000;
  if (ms > 60000)
    return 60000;
  return ms;
}

// ---------- Init ----------
void state_init()
{
  // Init telemetry — deliberately BEFORE the LittleFS mount below, not
  // after. This used to run here, AFTER littlefs_ok was already set from
  // LittleFS.begin()'s real result a few lines up — silently zeroing it
  // straight back to false on every single boot, even on a fully successful
  // mount. That's why the [FS DIAG] log could show "LittleFS.begin(false)
  // returned: true" and the .ino's halt gate still fire two lines later:
  // this memset was clobbering the result before setup() ever got to check
  // it. Moving it to the top of the function means nothing this function
  // writes later (littlefs_ok, or anything else added to VitalsState in the
  // future) can ever be wiped out by it again.
  memset(&currentSensors, 0, sizeof(currentSensors));
  memset(&currentVitals, 0, sizeof(currentVitals));

  // LittleFS (web assets)
  //
  // Diagnostic run BEFORE the mount attempt: independently confirm, via the
  // ESP-IDF partition API directly (not through LittleFS at all), that a
  // partition labeled "spiffs" actually exists in the partition table
  // currently flashed to this chip, and print its real offset/size. This
  // is deliberately independent of CORE_DEBUG_LEVEL/esp_littlefs's own
  // logging — if this block ever prints "NOT FOUND", the problem is the
  // partition table itself (wrong/stale table on the chip, or a label
  // mismatch), not the filesystem image content. If it prints a
  // size/address that doesn't match partitions.csv, the chip is still
  // running an old/different table than the one just flashed.
  {
    const esp_partition_t *fsPart = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (fsPart)
    {
      Serial.printf("[FS DIAG] Found partition label='spiffs' at offset=0x%06X size=0x%06X (%u bytes)\n",
                     fsPart->address, fsPart->size, fsPart->size);
    }
    else
    {
      Serial.println("[FS DIAG] NO partition labeled 'spiffs' found in the currently-flashed "
                      "partition table — this means the partition table on the chip does not "
                      "match partitions.csv, or a full chip erase + fresh upload is still needed.");
    }
  }

  // NOTE: Do NOT auto-format on a failed mount here. Silently retrying with
  // LittleFS.begin(true) used to mask a failed/empty mount by reporting
  // littlefs_ok = true even though the web asset partition was just wiped.
  // We now report the real mount result and let the halt gate in the .ino
  // (setup()) catch a bad mount and stop, instead of continuing silently
  // into a broken state.
  currentVitals.littlefs_ok = LittleFS.begin(false);
  Serial.printf("[FS DIAG] LittleFS.begin(false) returned: %s\n", currentVitals.littlefs_ok ? "true" : "false");

  // NVS
  if (!prefs.begin(NVS_NS, false)) {
      prefs.clear();
      prefs.begin(NVS_NS, false);
  }

  // WiFi
  loadStr(prefs, "wifi_ssid", currentConfig.wifi_ssid, sizeof(currentConfig.wifi_ssid), DEFAULT_WIFI_SSID);
  loadStr(prefs, "wifi_pass", currentConfig.wifi_pass, sizeof(currentConfig.wifi_pass), DEFAULT_WIFI_PASS);
  loadStr(prefs, "ap_pass", currentConfig.ap_pass, sizeof(currentConfig.ap_pass), DEFAULT_AP_PASS);

  // Firebase
  loadStr(prefs, "fb_api", currentConfig.fb_api_key, sizeof(currentConfig.fb_api_key), DEFAULT_FB_API_KEY);
  loadStr(prefs, "fb_proj", currentConfig.fb_project, sizeof(currentConfig.fb_project), DEFAULT_FB_PROJECT);
  loadStr(prefs, "fb_email", currentConfig.fb_email, sizeof(currentConfig.fb_email), DEFAULT_FB_EMAIL);
  loadStr(prefs, "fb_pass", currentConfig.fb_pass, sizeof(currentConfig.fb_pass), DEFAULT_FB_PASS);
  loadStr(prefs, "fb_col", currentConfig.fb_collection, sizeof(currentConfig.fb_collection), DEFAULT_FB_COLLECTION);
  loadStr(prefs, "dev_id", currentConfig.device_id, sizeof(currentConfig.device_id), DEFAULT_DEVICE_ID);

  // Timing. Clamped the same as save_intervals (command_handlers.cpp) so an old
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

  // Pins — plain GPIO numbers, kept independent of on/off state (see the
  // ConfigState::pin_* comment in state.h).
  currentConfig.pin_dht = prefs.getInt("pin_dht", PIN_DHT);
  currentConfig.pin_ds18b20 = prefs.getInt("pin_ds", PIN_DS18B20);
  currentConfig.pin_tds = prefs.getInt("pin_tds", PIN_TDS);
  currentConfig.pin_ph = prefs.getInt("pin_ph", PIN_PH);
  currentConfig.pin_lux_sda = prefs.getInt("pin_sda", PIN_LUX_SDA);
  currentConfig.pin_lux_scl = prefs.getInt("pin_scl", PIN_LUX_SCL);
  currentConfig.pin_wl = prefs.getInt("pin_wl", PIN_WL);
  currentConfig.pin_wl_power = prefs.getInt("pin_wlp", PIN_WL_PWR);

  // Feature flags — demo mode / Firebase upload gate.
  // We keep the keys extremely short to save NVS bytes, but map them to the full names from
  // config.h (demo, fb_en), so the code stays self-documenting.
  currentConfig.demo_mode        = prefs.getBool("demo", DEFAULT_DEMO_MODE);
  currentConfig.firebase_enabled = prefs.getBool("fb_en", DEFAULT_FIREBASE_ENABLED);

  // Feature flags — one NVS key per sensor: "en_0" .. "en_N". Each sensor's
  // default comes from DEFAULT_SENSOR_ENABLED[i] in config.h. This only
  // matters on first boot / after a factory reset; once a value is saved to
  // NVS, that saved value always wins over the compiled default.
  for (int i = 0; i < S_COUNT; ++i)
  {
    char k[8];
    snprintf(k, sizeof(k), "en_%d", i);
    currentConfig.sensor_enabled[i] = prefs.getBool(k, DEFAULT_SENSOR_ENABLED[i]);
  }

  // Crash/reboot diagnostics — mount the namespace and pull in whatever
  // reason was recorded on the PREVIOUS boot, before state_log_reset_reason()
  // (called from the .ino's setup(), right after this) overwrites it with
  // the current one. See the long comment on both functions in state.h.
  if (!crashPrefs.begin(CRASH_NVS_NS, false))
  {
    crashPrefs.clear();
    crashPrefs.begin(CRASH_NVS_NS, false);
  }
  loadStr(crashPrefs, "reason", s_lastResetReason, sizeof(s_lastResetReason), "");
}

// ---------- Save ----------
// Every Preferences::putX() call below returns the number of bytes it
// actually wrote — 0 means the write failed (full/worn/corrupted NVS
// partition, or the namespace handle isn't open in read-write mode). The
// old version of this function ignored every one of those return values
// and always reported success, so a failed save looked identical to a
// successful one from the caller's (and therefore the user's) point of
// view: the Web UI would cheerfully show "Saved!" while nothing had
// actually reached flash. We now AND every result together into `ok` and
// return that, so a single failed field fails the whole save — the
// command handlers that call state_save() surface this as "Failed to
// save" instead of a blind ack.
//
// Deliberately non-short-circuiting (plain `&=`, not `&&` with early
// return): every field still gets its write attempted even if an earlier
// one failed, so one bad key doesn't leave the rest silently unsaved too.
bool state_save()
{
  bool ok = true;

  ok &= prefs.putString("wifi_ssid", currentConfig.wifi_ssid) > 0 || strlen(currentConfig.wifi_ssid) == 0;
  ok &= prefs.putString("wifi_pass", currentConfig.wifi_pass) > 0 || strlen(currentConfig.wifi_pass) == 0;
  ok &= prefs.putString("ap_pass", currentConfig.ap_pass) > 0 || strlen(currentConfig.ap_pass) == 0;

  ok &= prefs.putString("fb_api", currentConfig.fb_api_key) > 0 || strlen(currentConfig.fb_api_key) == 0;
  ok &= prefs.putString("fb_proj", currentConfig.fb_project) > 0 || strlen(currentConfig.fb_project) == 0;
  ok &= prefs.putString("fb_email", currentConfig.fb_email) > 0 || strlen(currentConfig.fb_email) == 0;
  ok &= prefs.putString("fb_pass", currentConfig.fb_pass) > 0 || strlen(currentConfig.fb_pass) == 0;
  ok &= prefs.putString("fb_col", currentConfig.fb_collection) > 0 || strlen(currentConfig.fb_collection) == 0;
  ok &= prefs.putString("dev_id", currentConfig.device_id) > 0 || strlen(currentConfig.device_id) == 0;

  // Numeric/bool putX() calls write a fixed-size value that's never empty,
  // so unlike the strings above, 0 bytes written always means a real
  // failure here — no "empty string" carve-out needed.
  ok &= prefs.putUInt("int_read", currentConfig.interval_read_ms) > 0;
  ok &= prefs.putUInt("int_ws", currentConfig.interval_ws_ms) > 0;
  ok &= prefs.putUInt("int_vit", currentConfig.interval_vitals_ms) > 0;
  ok &= prefs.putUInt("int_fb", currentConfig.interval_fb_ms) > 0;

  ok &= prefs.putFloat("ph_off", currentConfig.ph_offset) > 0;
  ok &= prefs.putFloat("ph_slope", currentConfig.ph_slope) > 0;
  ok &= prefs.putFloat("tds_k", currentConfig.tds_k) > 0;

  // Pins — plain GPIO numbers (see the ConfigState::pin_* comment in state.h)
  ok &= prefs.putInt("pin_dht", currentConfig.pin_dht) > 0;
  ok &= prefs.putInt("pin_ds", currentConfig.pin_ds18b20) > 0;
  ok &= prefs.putInt("pin_tds", currentConfig.pin_tds) > 0;
  ok &= prefs.putInt("pin_ph", currentConfig.pin_ph) > 0;
  ok &= prefs.putInt("pin_sda", currentConfig.pin_lux_sda) > 0;
  ok &= prefs.putInt("pin_scl", currentConfig.pin_lux_scl) > 0;
  ok &= prefs.putInt("pin_wl", currentConfig.pin_wl) > 0;
  ok &= prefs.putInt("pin_wlp", currentConfig.pin_wl_power) > 0;

  for (int i = 0; i < S_COUNT; ++i)
  {
    char k[8];
    snprintf(k, sizeof(k), "en_%d", i);
    ok &= prefs.putBool(k, currentConfig.sensor_enabled[i]) > 0;
  }

  ok &= prefs.putBool("demo", currentConfig.demo_mode) > 0;
  ok &= prefs.putBool("fb_en", currentConfig.firebase_enabled) > 0;

  if (!ok)
  {
    webLog(0, LOG_ERR, "state_save: one or more NVS writes failed — settings may not be fully persisted.");
  }

  return ok;
}

// ---------- Factory reset ----------
void state_factory_reset()
{
  prefs.clear();
  prefs.end();
  // Factory reset means "wipe everything, including the admin password" —
  // unlike auth_reset() (BOOT button 10s hold), which deliberately leaves
  // Wi-Fi/sensors/calibration alone and only clears auth. A full factory
  // reset has no such carve-out: also clear the separate auth namespace so
  // the device comes back up fully "Unconfigured" on both fronts.
  authPrefs.begin(AUTH_NVS_NS, false);
  authPrefs.clear();
  authPrefs.end();
  delay(200);
  ESP.restart();
}

// ============================================================================
// Single-owner auth (admin password + session token)
// ============================================================================
// There is exactly one account. The username is hardcoded to "admin" in the
// WS auth handler (auth.cpp) and is never stored anywhere — only the
// password (plaintext, by design: this is a LAN/SoftAP-only local appliance
// with a single owner, not a multi-tenant networked service) and a random
// session token live in NVS, in their own namespace (see the comment on the
// declarations in state.h for why that separation matters).

// Generates a 32-character hex session token from the hardware RNG. Called
// on every successful login/password-set so a fresh token replaces any
// previous one — only the newest token a client was handed is ever valid.
String auth_issue_token()
{
  char buf[33];
  for (int i = 0; i < 32; i++)
  {
    uint8_t nibble = esp_random() & 0x0F;
    buf[i] = nibble < 10 ? ('0' + nibble) : ('a' + (nibble - 10));
  }
  buf[32] = '\0';

  strncpy(s_sessionToken, buf, sizeof(s_sessionToken) - 1);
  s_sessionToken[sizeof(s_sessionToken) - 1] = '\0';
  authPrefs.putString("token", s_sessionToken);

  return String(s_sessionToken);
}

void auth_init()
{
  if (!authPrefs.begin(AUTH_NVS_NS, false))
  {
    // Same self-heal pattern as the main NVS namespace in state_init():
    // a corrupted auth partition should never brick login entirely.
    authPrefs.clear();
    authPrefs.begin(AUTH_NVS_NS, false);
  }

  loadStr(authPrefs, "admin_pass", s_adminPass, sizeof(s_adminPass), DEFAULT_ADMIN_PASS);
  loadStr(authPrefs, "token", s_sessionToken, sizeof(s_sessionToken), "");

  // If DEFAULT_ADMIN_PASS was set in secrets.h (a shipped default password,
  // as opposed to the "" empty/Unconfigured default), persist it to NVS on
  // this first boot so it survives being cleared out of secrets.h later —
  // same "fallback becomes the real saved value" pattern loadStr()'s callers
  // already rely on for Wi-Fi/Firebase credentials in state_init().
  if (!authPrefs.isKey("admin_pass") && strlen(s_adminPass) > 0)
  {
    authPrefs.putString("admin_pass", s_adminPass);
  }
}

bool auth_is_configured()
{
  return strlen(s_adminPass) > 0;
}

bool auth_check_password(const String &candidate)
{
  if (!auth_is_configured())
    return false; // nothing to check against — caller should be using set_password instead
  return candidate.length() > 0 && candidate.equals(s_adminPass);
}

void auth_set_password(const String &newPass)
{
  strncpy(s_adminPass, newPass.c_str(), sizeof(s_adminPass) - 1);
  s_adminPass[sizeof(s_adminPass) - 1] = '\0';
  authPrefs.putString("admin_pass", s_adminPass);
  // A password change/first-time-set invalidates any previously issued
  // session token — old browser sessions must re-authenticate against the
  // new password rather than silently staying logged in on the old one.
  auth_issue_token();
}

bool auth_check_token(const String &candidate)
{
  if (strlen(s_sessionToken) == 0 || candidate.length() == 0)
    return false;
  return candidate.equals(s_sessionToken);
}

// BOOT button 10-second hold — wipes ONLY the admin password + session
// token. Wi-Fi, sensors, pins, and calibration are untouched: this call
// never goes near the "hygrow" namespace/currentConfig at all.
void auth_reset()
{
  authPrefs.remove("admin_pass");
  authPrefs.remove("token");
  s_adminPass[0] = '\0';
  s_sessionToken[0] = '\0';
}

// ============================================================================
// Crash / reboot diagnostics
// ============================================================================
// Persists the CURRENT boot's reset reason to NVS so it survives to become
// the "last reset reason" state_get_last_reset_reason() returns on the NEXT
// boot. Called once from the .ino's setup(), immediately after state_init()
// has already captured the previous value into s_lastResetReason above.
void state_log_reset_reason(const char *reason)
{
  if (!reason) return;
  crashPrefs.putString("reason", reason);
}

String state_get_last_reset_reason()
{
  return String(s_lastResetReason);
}
