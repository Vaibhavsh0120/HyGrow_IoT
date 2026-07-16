/*
 * ============================================================================
 * sensors.h — Unified Hardware Interface
 * ============================================================================
 */
#pragma once
#include "../core/state.h"

// ── Initialization Functions ──
bool init_wl();
bool init_light();
bool init_tds();
bool init_dht();
bool init_ph();
bool init_wtemp();

// ── Read Functions ──
float read_wl();
float read_light();
float read_tds(float water_temp_c);
bool  read_dht(float &temp, float &hum);
float read_ph(float water_temp_c);
bool  read_wtemp(float &temp);

// ── Main Orchestrator ──
// Initializes all active sensors
void sensors_init();

// Reads all physical sensors sequentially to avoid ADC ground-loop interference
void sensors_read_all(SensorData &data, const ConfigState &cfg);
