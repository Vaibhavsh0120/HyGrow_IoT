/*
 * ============================================================================
 * state.h — Global State & IPC (Inter-Process Communication)
 * ============================================================================
 */

#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../../config.h"

// ─────────────────────────────────────────────────────────────────────────────
//  System Configuration (Saved to NVS)
// ─────────────────────────────────────────────────────────────────────────────
struct ConfigState {
    char wifi_ssid[32];
    char wifi_password[64];
    bool demo_mode;
    bool sensor_enabled[S_COUNT];
};

// ─────────────────────────────────────────────────────────────────────────────
//  Live Sensor Data (Updated by Core 1, Read by Core 0)
// ─────────────────────────────────────────────────────────────────────────────
struct SensorData {
    float wl_percent;           // Water Level (0-100%)
    float light_lux;            // BH1750 (Lux)
    float tds_ppm;              // TDS (ppm)
    float dht_temp;             // DHT22 Air Temp (°C)
    float dht_hum;              // DHT22 Humidity (%)
    float vpd_kpa;              // Vapor Pressure Deficit (kPa)
    float ph_val;               // pH (0-14)
    float w_temp;               // DS18B20 Water Temp (°C)
    bool  errors[S_COUNT];      // True if sensor is disconnected/failed
};

// Global Instances (Protected by stateMutex)
extern ConfigState currentConfig;
extern SensorData  currentData;
extern SemaphoreHandle_t stateMutex;

// Core Functions
void state_init();
void state_save();

// Cross-core logging function (Sends logs to Web UI Terminal)
void webLog(const char* format, ...);
