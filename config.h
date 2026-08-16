// ================================================================
//  HyGrow_IoT — config.h
//  Compile-time defaults. Anything marked [NVS] can be overridden
//  at runtime from the Web Doctor Settings page and is persisted
//  in NVS (namespace "hygrow"). Values here are only the fallback
//  used on first boot or after a factory reset.
// ================================================================
#ifndef HYGROW_CONFIG_H
#define HYGROW_CONFIG_H

#include <Arduino.h>

// ---------- Required secrets.h ----------
// secrets.h is NOT optional. This mirrors the LittleFS filesystem image:
// both are external inputs the firmware cannot safely invent a default for,
// so both fail loudly rather than silently substituting something that
// looks like it works. LittleFS's check happens at RUNTIME (state_init(),
// state.cpp) because a missing/corrupt filesystem image can only be
// detected once the device is actually running. secrets.h's check happens
// here, at COMPILE TIME, because whether the file exists is already known
// before a single line of firmware runs — failing the build outright is
// strictly better than a runtime halt: no board ever gets flashed with
// credentials it doesn't have, and the missing file is caught in seconds on
// a dev machine instead of after a trip out to the hardware.
//
// This used to be __has_include-guarded with a full set of safe empty-string
// FALLBACK_* defaults defined right below when the file was absent — meaning
// a clone with no secrets.h at all would compile and boot completely
// normally into "Unconfigured" (blank WiFi/Firebase, no admin password).
// That's exactly the inconsistency being removed: every credential now has
// exactly one source (secrets.h), never two (secrets.h with a config.h
// fallback underneath it), so there is never a question of which one a
// given boot actually got its value from.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "secrets.h is missing. Copy example.secrets.h to secrets.h in the project root and fill in your own WiFi/AP/admin/Firebase credentials before building — see example.secrets.h for what's required. There is no fallback: this firmware will not compile without it."
#endif

// Every FALLBACK_* macro below MUST already be defined by secrets.h — none
// of them get a silent default here. A single #ifndef will name exactly
// which one is missing (and fail the build) instead of every one of them
// quietly resolving to "" the way a config.h-side fallback used to.
#ifndef FALLBACK_WIFI_SSID
#error "secrets.h is missing FALLBACK_WIFI_SSID — see example.secrets.h."
#endif
#ifndef FALLBACK_WIFI_PASS
#error "secrets.h is missing FALLBACK_WIFI_PASS — see example.secrets.h."
#endif
#ifndef FALLBACK_AP_PASS
#error "secrets.h is missing FALLBACK_AP_PASS — see example.secrets.h."
#endif
#ifndef FALLBACK_ADMIN_PASS
#error "secrets.h is missing FALLBACK_ADMIN_PASS — see example.secrets.h. Set it to \"\" explicitly if you want the device to boot Unconfigured (Set Password modal) rather than omitting it."
#endif
#ifndef FALLBACK_FIREBASE_API_KEY
#error "secrets.h is missing FALLBACK_FIREBASE_API_KEY — see example.secrets.h."
#endif
#ifndef FALLBACK_FIREBASE_PROJECT_ID
#error "secrets.h is missing FALLBACK_FIREBASE_PROJECT_ID — see example.secrets.h."
#endif
#ifndef FALLBACK_FIREBASE_USER_EMAIL
#error "secrets.h is missing FALLBACK_FIREBASE_USER_EMAIL — see example.secrets.h."
#endif
#ifndef FALLBACK_FIREBASE_USER_PASSWORD
#error "secrets.h is missing FALLBACK_FIREBASE_USER_PASSWORD — see example.secrets.h."
#endif
#ifndef FALLBACK_FIRESTORE_COLLECTION
#error "secrets.h is missing FALLBACK_FIRESTORE_COLLECTION — see example.secrets.h."
#endif
#ifndef FALLBACK_DEVICE_ID
#error "secrets.h is missing FALLBACK_DEVICE_ID — see example.secrets.h."
#endif
#ifndef FALLBACK_DEMO_MODE
#error "secrets.h is missing FALLBACK_DEMO_MODE — see example.secrets.h. Set it to true or false explicitly."
#endif

// ---------- Identity & cloud ----------
#define DEFAULT_DEVICE_ID FALLBACK_DEVICE_ID                     // [NVS] dev_id
#define DEFAULT_FIRESTORE_COLLECTION FALLBACK_FIRESTORE_COLLECTION // [NVS] fb_col
#define DEFAULT_ADMIN_PASS FALLBACK_ADMIN_PASS                   // [NVS] admin_pass (auth namespace) — "" = unconfigured
#define SERIAL_BAUD_RATE 115200                                  // compile-time only

// ---------- WiFi / AP fallback ----------
#define DEFAULT_STA_TIMEOUT_MS 15000                       // compile-time
#define DEFAULT_AP_SSID "HyGrow-Setup"                     // compile-time
#define DEFAULT_AP_PASSWORD FALLBACK_AP_PASS               // [NVS] ap_pass (min 8 chars)
#define DEFAULT_WIFI_SSID FALLBACK_WIFI_SSID               // [NVS] wifi_ssid
#define DEFAULT_WIFI_PASS FALLBACK_WIFI_PASS               // [NVS] wifi_pass
#define DEFAULT_AP_PASS DEFAULT_AP_PASSWORD                // [NVS] ap_pass
#define DEFAULT_FB_API_KEY FALLBACK_FIREBASE_API_KEY       // [NVS] fb_api
#define DEFAULT_FB_PROJECT FALLBACK_FIREBASE_PROJECT_ID    // [NVS] fb_proj
#define DEFAULT_FB_EMAIL FALLBACK_FIREBASE_USER_EMAIL      // [NVS] fb_email
#define DEFAULT_FB_PASS FALLBACK_FIREBASE_USER_PASSWORD    // [NVS] fb_pass
#define DEFAULT_FB_COLLECTION DEFAULT_FIRESTORE_COLLECTION // [NVS] fb_col

// ---------- Timing (all in ms) ----------
#define DEFAULT_INTERVAL_READ_MS 2000   // [NVS] int_read  — sensor sampling
#define DEFAULT_INTERVAL_WS_MS 1000     // [NVS] int_ws    — WS data push
#define DEFAULT_INTERVAL_VITALS_MS 1000 // [NVS] int_vit   — vitals push (ConfigState.interval_vitals_ms)
#define DEFAULT_INTERVAL_FB_MS 10000    // [NVS] int_fb    — Firestore patch

// ---------- Pin assignments (ESP32-S3 N16R8) ----------
// [NVS] pin_*  — changes take effect after reboot.
//
// Pin numbers are always a plain GPIO assignment — they are NOT the on/off
// switch for a sensor. Whether a sensor is actually read is controlled by
// exactly one flag: sensor_enabled[] (see DEFAULT_SENSOR_ENABLED below and
// ConfigState::sensor_enabled in state.h). A pin's saved value is kept and
// shown even while its sensor is disabled, so turning a sensor off never
// erases which GPIO it's wired to.
//
// FORBIDDEN: GPIO19 (USB D-) and GPIO20 (USB D+) — with build_flags
// -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1 (see platformio.ini),
// Serial *is* the native USB peripheral on these two pins. Calling
// pinMode()/analogRead() on either one fights the USB stack for the same
// lines and reads as a repeating "board disconnects on its own" while a
// serial monitor is attached. Never assign a sensor/LED pin here to 19 or 20.
#define DEFAULT_PIN_WL_SIG 1   // Water level analog signal
#define DEFAULT_PIN_WL_PWR 5   // Water level power gate (reduces electrolysis)
#define DEFAULT_PIN_I2C_SDA 8  // BH1750 SDA
#define DEFAULT_PIN_I2C_SCL 9  // BH1750 SCL
#define DEFAULT_PIN_TDS_SIG 2  // TDS analog signal (ADC1)
#define DEFAULT_PIN_DHT22 6    // DHT22 data
#define DEFAULT_PIN_PH_SIG 7   // pH analog signal (ADC1 — also avoids the ADC2/WiFi contention pin 20 had)
#define DEFAULT_PIN_DS18B20 4  // OneWire bus for DS18B20
#define DEFAULT_PIN_RGB_LED 48 // WS2812 status LED

// Compatibility aliases used by the runtime config layer.
#define PIN_DHT DEFAULT_PIN_DHT22
#define PIN_DS18B20 DEFAULT_PIN_DS18B20
#define PIN_TDS DEFAULT_PIN_TDS_SIG
#define PIN_PH DEFAULT_PIN_PH_SIG
#define PIN_LUX_SDA DEFAULT_PIN_I2C_SDA
#define PIN_LUX_SCL DEFAULT_PIN_I2C_SCL
#define PIN_WL DEFAULT_PIN_WL_SIG
#define PIN_WL_PWR DEFAULT_PIN_WL_PWR
#ifndef PIN_RGB_LED
#define PIN_RGB_LED DEFAULT_PIN_RGB_LED
#endif

// ---------- Calibration defaults ----------
// [NVS] ph_off / ph_slope  — linear model: pH = slope * raw_volt + offset
#define DEFAULT_PH_OFFSET 0.0f
#define DEFAULT_PH_SLOPE -5.70f // typical for a 5V probe on 3.3V ADC
// [NVS] tds_k  — scale factor applied on top of the polynomial output
#define DEFAULT_TDS_K 1.0f

// ---------- Sensor IDs ----------
// Order MUST match ERROR_COLORS[] in src/utils/led_status.cpp, and is
// mirrored by TAB_TO_SENSOR_ID's string keys in data/js/app.js (JS uses
// short string ids, not this numeric enum, so there's no direct ordering
// dependency there — but keep them conceptually aligned when adding sensors).
enum SensorID
{
    S_WL = 0,
    S_LIGHT = 1,
    S_TDS = 2,
    S_DHT = 3,
    S_PH = 4,
    S_WTEMP = 5,
    S_COUNT = 6,
    S_FIREBASE = 99 // pseudo-id used only for LED error signalling
};

// ---------- Feature flags ----------
#define DEFAULT_DEMO_MODE FALLBACK_DEMO_MODE // [NVS] demo — see secrets.h
#define DEFAULT_FIREBASE_ENABLED true         // [NVS] fb_en

// ---------- Demo mode pin sentinel ----------
// A sentinel GPIO number, never a real pin, assigned to every sensor's pin
// field(s) while demo_mode is on. This is what makes demo mode a genuine
// per-sensor hardware-layer fact rather than a UI-only concept: a sensor's
// pin equals DEMO_MODE_PIN if and only if that sensor is currently sourcing
// simulated data (see sensorPinIsDemo() in task_sensor.cpp). Any negative
// value is already treated as "not a real GPIO, never conflicts" throughout
// this codebase (see isForbiddenPin()/validatePinSet() in
// command_handlers.cpp), so this slots into the existing pin-safety system
// with zero risk of colliding with a real assignment. -42 is arbitrary but
// memorable and unambiguous in logs/JSON payloads — it can never be mistaken
// for a real GPIO number or for the "unset" sentinel some libraries use (-1).
#define DEMO_MODE_PIN (-42)

// [NVS] s_en_<i> (per sensor) — the SINGLE on/off switch for each sensor,
// indexed the same way as the SensorID enum above (S_WL, S_LIGHT, S_TDS,
// S_DHT, S_PH, S_WTEMP). This is the only thing that turns a sensor's
// reading/upload on or off — pin numbers are a separate, purely physical
// GPIO assignment and are never used to infer on/off state. Only used on
// first boot / after a factory reset; once a value is saved to NVS, that
// saved value always wins over this default.
//
// Every sensor ships ON except pH (S_PH), which ships OFF: pH needs a
// probe calibrated in real liquid to read anything meaningful, so it stays
// disabled until the user calibrates it and switches it on themselves.
static constexpr bool DEFAULT_SENSOR_ENABLED[S_COUNT] = {
    true,  // S_WL
    true,  // S_LIGHT
    true,  // S_TDS
    true,  // S_DHT
    false, // S_PH — off by default, see note above
    true,  // S_WTEMP
};

// ---------- Log levels (for WS log frames) ----------
#define LOG_INFO 0
#define LOG_WARN 1
#define LOG_ERR 2

// ---------- Sanity checks ----------
static_assert(S_COUNT == 6, "S_COUNT must stay in sync with SensorID enum");
static_assert(sizeof(DEFAULT_SENSOR_ENABLED) / sizeof(DEFAULT_SENSOR_ENABLED[0]) == S_COUNT,
              "DEFAULT_SENSOR_ENABLED must have exactly S_COUNT entries");

// FALLBACK_AP_PASS now comes ONLY from secrets.h (no config.h fallback
// underneath it any more — see the "Required secrets.h" block above), so a
// value that's too short is no longer caught by anything else. WiFi.softAP()
// (task_network.cpp) silently fails to start a WPA2 AP for any non-empty
// password under 8 characters, which would only surface on hardware as "the
// SoftAP recovery network never appears" — exactly the kind of failure this
// whole secrets.h change is meant to catch at compile time instead.
static_assert(sizeof(FALLBACK_AP_PASS) - 1 == 0 || sizeof(FALLBACK_AP_PASS) - 1 >= 8,
              "FALLBACK_AP_PASS in secrets.h must be empty (\"\", open network) or at least 8 characters (WPA2 minimum) — a shorter non-empty password makes WiFi.softAP() fail silently at runtime.");

#endif // HYGROW_CONFIG_H
