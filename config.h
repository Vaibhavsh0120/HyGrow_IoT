/*
 * ============================================================================
 * config.h — Central Configuration for HyGrow-IoT (ESP32-S3)
 * ============================================================================
 */

#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────────────────────────────────────
//  System Configuration
// ─────────────────────────────────────────────────────────────────────────────
#define FIRESTORE_COLLECTION   "sensor_readings"
#define DEVICE_ID              "ESP32S3_001"
#define SERIAL_BAUD_RATE       115200

// ─────────────────────────────────────────────────────────────────────────────
//  Sensor Pin Assignments (ESP32-S3 N16R8)
// ─────────────────────────────────────────────────────────────────────────────
// Water Level Sensor (Analog resistive)
#define PIN_WL_SIG             1    // Analog input (ADC1)
#define PIN_WL_PWR             5    // Digital output to power sensor

// BH1750 Light Sensor (I2C)
#define PIN_I2C_SDA            8    // I2C Data
#define PIN_I2C_SCL            9    // I2C Clock

// TDS Water Quality Sensor (Analog)
#define PIN_TDS_SIG            2    // Analog input (ADC1)

// DHT22 Temperature & Humidity Sensor (Digital)
#define PIN_DHT22              6    // Digital data pin

// pH Sensor (Analog)
#define PIN_PH_SIG             20   // Analog input (ADC2)

// DS18B20 Waterproof Temperature Sensor (OneWire)
#define PIN_DS18B20            4    // OneWire data pin (Needs 4.7kΩ pull-up)

// Built-in RGB LED (NeoPixel)
#define PIN_RGB_LED            48   // GPIO 48

// ─────────────────────────────────────────────────────────────────────────────
//  Sensor Identification Enums
// ─────────────────────────────────────────────────────────────────────────────
// Used for array indexing, UI toggles, and LED error colors
enum SensorID {
    S_WL = 0,         // Water Level
    S_LIGHT,          // BH1750
    S_TDS,            // TDS Quality
    S_DHT,            // DHT22 Air Temp/Hum
    S_PH,             // pH Sensor
    S_WTEMP,          // DS18B20 Water Temp
    S_COUNT,          // Total number of sensors
    S_FIREBASE = 99   // Special ID for Firebase errors
};
