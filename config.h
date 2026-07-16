/*
 * ============================================================================
 *  config.h — Central Configuration for ESP32-S3 Sensor → Firestore Pipeline
 * ============================================================================
 *
 *  All user-configurable settings live here. Edit this file before uploading.
 *
 *  Board:  ESP32-S3 N16R8
 *  Author: Firebase_Demo Project
 * ============================================================================
 */

#ifndef CONFIG_H
#define CONFIG_H

// ─────────────────────────────────────────────────────────────────────────────
//  Operation Mode
// ─────────────────────────────────────────────────────────────────────────────
#define DEMO_MODE             1       // Set to 1 for Demo Mode (mock data), 0 for Real Sensors
#define ENABLE_WEB_DIAGNOSE   1       // Set to 1 to enable the local Web UI on port 80

// ─────────────────────────────────────────────────────────────────────────────
//  WiFi Credentials
// ─────────────────────────────────────────────────────────────────────────────
#define WIFI_SSID       "Vaibhav"
#define WIFI_PASSWORD   "VaibhavS"

// ─────────────────────────────────────────────────────────────────────────────
//  Firebase / Firestore Credentials
// ─────────────────────────────────────────────────────────────────────────────
//  Get these from Firebase Console → Project Settings
#define FIREBASE_API_KEY      "AIzaSyAsW3UdiapN41zCfAd5Wi_kQNzLzojeORk"
#define FIREBASE_PROJECT_ID   "farm-help-383f1"

//  Create a user in Firebase Console → Authentication → Add User
#define FIREBASE_USER_EMAIL    "vaibhavsh0120@gmail.com"
#define FIREBASE_USER_PASSWORD "12345678"

//  Firestore collection name where sensor data will be stored
#define FIRESTORE_COLLECTION   "sensor_readings"

//  Device identifier (useful if you have multiple ESP32 devices)
#define DEVICE_ID              "ESP32S3_001"

// ─────────────────────────────────────────────────────────────────────────────
//  Timing Configuration
// ─────────────────────────────────────────────────────────────────────────────
#define SENSOR_READ_INTERVAL_MS   2000    // Read & send every 2 seconds
#define WIFI_CONNECT_TIMEOUT_MS   15000   // WiFi connection timeout
#define WIFI_RETRY_DELAY_MS       500     // Delay between WiFi retries
#define FIREBASE_READY_TIMEOUT_MS 10000   // Firebase auth timeout

// ─────────────────────────────────────────────────────────────────────────────
//  RGB LED Configuration (Built-in NeoPixel on ESP32-S3)
// ─────────────────────────────────────────────────────────────────────────────
#define RGB_LED_PIN       48    // GPIO 48 — built-in WS2812 NeoPixel
#define RGB_LED_COUNT     1     // Single LED
#define RGB_LED_BRIGHTNESS 30   // 0-255, keep low to avoid blinding

// ─────────────────────────────────────────────────────────────────────────────
//  Sensor Pin Assignments
//  NOTE: Only ADC1 pins used (ADC2 unavailable when WiFi is active on ESP32-S3)
// ─────────────────────────────────────────────────────────────────────────────

// Water Level Sensor — Analog resistive
#define WATER_LEVEL_SIGNAL_PIN  1    // Analog input (ADC1)
#define WATER_LEVEL_POWER_PIN   5    // Digital output to power sensor

// BH1750 Light Sensor — I2C
#define I2C_SDA_PIN             8    // I2C Data
#define I2C_SCL_PIN             9    // I2C Clock
#define BH1750_I2C_ADDR         0x23 // Default address (ADDR pin LOW)

// TDS Water Quality Sensor — Analog
#define TDS_SENSOR_PIN          2    // Analog input (ADC1)
#define TDS_AREF_VOLTAGE        3.3  // ESP32-S3 reference voltage
#define TDS_ADC_RANGE           4096 // 12-bit ADC

// DHT22 Temperature & Humidity Sensor — Digital
#define DHT22_DATA_PIN          6    // Digital data pin
#define DHT_SENSOR_TYPE         DHT22

// pH Sensor — Analog
#define PH_SENSOR_PIN           20   // Analog input (ADC2) - WARNING: May conflict with WiFi!
#define PH_AREF_VOLTAGE         3.3  // ESP32-S3 reference voltage
#define PH_ADC_RANGE            4096 // 12-bit ADC

// DS18B20 Waterproof Temperature Sensor — OneWire
#define DS18B20_DATA_PIN        4    // OneWire data pin (needs 4.7kΩ pull-up)

// ─────────────────────────────────────────────────────────────────────────────
//  Sensor Identification (used for LED error colors & logging)
// ─────────────────────────────────────────────────────────────────────────────
enum SensorID {
    SENSOR_WATER_LEVEL = 0,
    SENSOR_LIGHT       = 1,
    SENSOR_TDS         = 2,
    SENSOR_DHT22       = 3,
    SENSOR_PH          = 4,
    SENSOR_WATER_TEMP  = 5,
    SENSOR_COUNT       = 6,   // Total number of sensors
    SENSOR_FIREBASE    = 99   // Special ID for Firebase errors
};

// ─────────────────────────────────────────────────────────────────────────────
//  Data Structure — holds all sensor readings for one cycle
// ─────────────────────────────────────────────────────────────────────────────
struct SensorData {
    // Water Level
    int   waterLevelRaw;        // 0–4095 raw ADC
    float waterLevelPercent;    // 0–100%

    // BH1750 Light
    float lightLux;             // 0–65535 lux

    // TDS
    float tdsPPM;               // ppm

    // DHT22
    float airTempC;             // °C
    float humidityPercent;      // %
    float vpdKpa;               // Vapor Pressure Deficit (kPa)

    // pH
    float phValue;              // 0–14

    // DS18B20 Water Temp
    float waterTempC;           // °C

    // Error flags per sensor
    bool sensorError[SENSOR_COUNT];
};

// ─────────────────────────────────────────────────────────────────────────────
//  Serial Debug
// ─────────────────────────────────────────────────────────────────────────────
#define SERIAL_BAUD_RATE  115200
#define DEBUG_PRINT       true   // Set to false to disable serial debug output

#if DEBUG_PRINT
  #define DBG(x)    Serial.print(x)
  #define DBGLN(x)  Serial.println(x)
  #define DBGF(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG(x)
  #define DBGLN(x)
  #define DBGF(...)
#endif

#endif // CONFIG_H
