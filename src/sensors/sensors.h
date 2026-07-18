/*
 * ============================================================================
 * sensors.h — Unified Hardware Interface
 * ============================================================================
 */
#pragma once
#include "../core/state.h"

// ── Initialization Functions ──
// init_light() returns true only if a BH1750 actually ACKed on the I2C
// bus, so the orchestrator can auto-disable it when nothing is wired up.
// The others stay void: they have no reliable "is it there" probe at
// init time (they already degrade gracefully per-read via errors[]).
void init_wl();
bool init_light();
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
