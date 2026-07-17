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

// ---------- Identity & cloud ----------
#define DEFAULT_DEVICE_ID            "ESP32S3_001"        // [NVS] dev_id
#define DEFAULT_FIRESTORE_COLLECTION "hygrow_devices"     // [NVS] fb_col
#define SERIAL_BAUD_RATE             115200               // compile-time only

// ---------- Firmware version ----------
// Bump this on every release. Reported in vitals so the web UI can show it.
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 1
#define FW_VERSION_PATCH 0
#define FW_VERSION_STRING "1.1.0"

// ---------- WiFi / AP fallback ----------
#define DEFAULT_STA_TIMEOUT_MS       15000                // compile-time
#define DEFAULT_AP_SSID              "HyGrow-Setup"       // compile-time
#define DEFAULT_AP_PASSWORD          "hygrow1234"         // [NVS] ap_pass (min 8 chars)

// ---------- Timing (all in ms) ----------
#define DEFAULT_INTERVAL_READ_MS     2000                 // [NVS] int_read  — sensor sampling
#define DEFAULT_INTERVAL_WS_MS       1000                 // [NVS] int_ws    — WS data push
#define DEFAULT_INTERVAL_VITALS_MS   1000                 // [NVS] int_vit   — vitals push
#define DEFAULT_INTERVAL_FB_MS       10000                // [NVS] int_fb    — Firestore patch

// ---------- Pin assignments (ESP32-S3 N16R8) ----------
// [NVS] pin_*  — changes take effect after reboot.
#define DEFAULT_PIN_WL_SIG    1     // Water level analog signal
#define DEFAULT_PIN_WL_PWR    5     // Water level power gate (reduces electrolysis)
#define DEFAULT_PIN_I2C_SDA   8     // BH1750 SDA
#define DEFAULT_PIN_I2C_SCL   9     // BH1750 SCL
#define DEFAULT_PIN_TDS_SIG   2     // TDS analog signal (ADC1)
#define DEFAULT_PIN_DHT22     6     // DHT22 data
#define DEFAULT_PIN_PH_SIG    20    // pH analog signal (ADC2 — WiFi may disturb)
#define DEFAULT_PIN_DS18B20   4     // OneWire bus for DS18B20
#define DEFAULT_PIN_RGB_LED   48    // WS2812 status LED

// ---------- Calibration defaults ----------
// [NVS] ph_off / ph_slope  — linear model: pH = slope * raw_volt + offset
#define DEFAULT_PH_OFFSET     0.0f
#define DEFAULT_PH_SLOPE      -5.70f   // typical for a 5V probe on 3.3V ADC
// [NVS] tds_k  — scale factor applied on top of the polynomial output
#define DEFAULT_TDS_K         1.0f

// ---------- Feature flags ----------
#define DEFAULT_DEMO_MODE         false   // [NVS] demo
#define DEFAULT_FIREBASE_ENABLED  true    // [NVS] fb_en
#define DEFAULT_OTA_ENABLED       true    // [NVS] ota_en
#define DEFAULT_SENSOR_ENABLED    true    // [NVS] s_en_<i> (per sensor)

// ---------- Sensor IDs ----------
// Order MUST match SENSOR_NAMES[] in sensors.cpp AND SENSOR_CONFIG in app.js.
enum SensorID {
    S_WL     = 0,
    S_LIGHT  = 1,
    S_TDS    = 2,
    S_DHT    = 3,
    S_PH     = 4,
    S_WTEMP  = 5,
    S_COUNT  = 6,
    S_FIREBASE = 99   // pseudo-id used only for LED error signalling
};

// ---------- Log levels (for WS log frames) ----------
#define LOG_INFO  0
#define LOG_WARN  1
#define LOG_ERR   2

// ---------- Sanity checks ----------
static_assert(S_COUNT == 6, "S_COUNT must stay in sync with SensorID enum");

#endif // HYGROW_CONFIG_H
