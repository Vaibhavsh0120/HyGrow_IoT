/*
 * ============================================================================
 * sensors.h — Unified Hardware Interface
 * ============================================================================
 */
#pragma once
#include "../core/state.h"

// ── Lightweight data container used by the shared orchestrator ──
struct SensorData
{
    float w_temp = 0.0f;
    float tds_ppm = 0.0f;
    float dht_temp = 0.0f;
    float dht_hum = 0.0f;
    float light_lux = 0.0f;
    float ph_val = 0.0f;
    float wl_percent = 0.0f;
    bool errors[S_COUNT] = {false};
};

// ── Initialization Functions ──
void init_wl();
void init_light();
void init_tds();
void init_dht();
void init_ph();
void init_wtemp();

// ── Read Functions ──
float read_wl();
float read_light();
float read_tds(float water_temp_c);
bool read_dht(float &temp, float &hum);
float read_ph(float water_temp_c);
bool read_wtemp(float &temp);

// ── Main Orchestrator ──
// Initializes all active sensors
void sensors_init();
void sensors_init_all();

// Reads all physical sensors sequentially to avoid ADC ground-loop interference
void sensors_read_all(SensorData &data, const ConfigState &cfg);
