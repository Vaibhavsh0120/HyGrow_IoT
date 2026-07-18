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

// ---------- Optional secrets.h ----------
// secrets.h is gitignored and only used as a first-boot / factory-reset
// fallback (see example.secrets.h). __has_include lets this compile fine
// even if the file has never been created — in that case every FALLBACK_*
// macro below is given a safe empty/default value instead.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef FALLBACK_WIFI_SSID
#define FALLBACK_WIFI_SSID ""
#endif
#ifndef FALLBACK_WIFI_PASS
#define FALLBACK_WIFI_PASS ""
#endif
#ifndef FALLBACK_AP_PASS
#define FALLBACK_AP_PASS "hygrow1234"
#endif
#ifndef FALLBACK_FIREBASE_API_KEY
#define FALLBACK_FIREBASE_API_KEY ""
#endif
#ifndef FALLBACK_FIREBASE_PROJECT_ID
#define FALLBACK_FIREBASE_PROJECT_ID ""
#endif
#ifndef FALLBACK_FIREBASE_USER_EMAIL
#define FALLBACK_FIREBASE_USER_EMAIL ""
#endif
#ifndef FALLBACK_FIREBASE_USER_PASSWORD
#define FALLBACK_FIREBASE_USER_PASSWORD ""
#endif
#ifndef FALLBACK_FIRESTORE_COLLECTION
#define FALLBACK_FIRESTORE_COLLECTION "sensor_data"
#endif
#ifndef FALLBACK_DEVICE_ID
#define FALLBACK_DEVICE_ID "ESP32S3_001"
#endif

// ---------- Identity & cloud ----------
#define DEFAULT_DEVICE_ID FALLBACK_DEVICE_ID                     // [NVS] dev_id
#define DEFAULT_FIRESTORE_COLLECTION FALLBACK_FIRESTORE_COLLECTION // [NVS] fb_col
#define SERIAL_BAUD_RATE 115200                                  // compile-time only

// ---------- Firmware version ----------
// Bump this on every release. Reported in vitals so the web UI can show it.
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 1
#define FW_VERSION_PATCH 0
#define FW_VERSION_STRING "1.1.0"

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

// ---------- Feature flags ----------
#define DEFAULT_DEMO_MODE false       // [NVS] demo
#define DEFAULT_FIREBASE_ENABLED true // [NVS] fb_en
#define DEFAULT_SENSOR_ENABLED true   // [NVS] s_en_<i> (per sensor) — applies to every
                                       // sensor EXCEPT pH, which has its own override
                                       // below. pH needs a calibrated probe in real
                                       // liquid to read anything meaningful, so it
                                       // ships off by default; every other sensor
                                       // ships on.
#define DEFAULT_PH_SENSOR_ENABLED false // [NVS] s_en_4 (S_PH) — off by default

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

// ---------- Log levels (for WS log frames) ----------
#define LOG_INFO 0
#define LOG_WARN 1
#define LOG_ERR 2

// ---------- Sanity checks ----------
static_assert(S_COUNT == 6, "S_COUNT must stay in sync with SensorID enum");

#endif // HYGROW_CONFIG_H
