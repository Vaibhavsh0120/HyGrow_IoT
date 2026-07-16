/*
 * ============================================================================
 *  sensor_ph.h — DFRobot Gravity Lab pH Sensor V2
 * ============================================================================
 *  
 *  Sensor: DFRobot Gravity Lab Grade Analog pH Sensor Meter Kit V2
 *  Interface: Analog read (ADC1 pin)
 *  Error Color: 🔵 Blue
 *  
 *  Measures pH value (0–14 scale).
 *  Uses manual voltage-to-pH conversion with configurable calibration
 *  constants for reliable operation on ESP32-S3.
 *  
 *  Calibration:
 *    The sensor outputs a voltage proportional to pH.
 *    At pH 7.0 (neutral): ~1.5V (at 3.3V Vref)
 *    At pH 4.0 (acid):    ~2.0V
 *    These values can be tuned via the calibration constants below.
 * ============================================================================
 */

#ifndef SENSOR_PH_H
#define SENSOR_PH_H

#include <Arduino.h>
#include "../../config.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Calibration Constants (adjust after calibrating with buffer solutions)
// ─────────────────────────────────────────────────────────────────────────────
//  Measure voltage at pH 7.0 buffer → set PH_VOLTAGE_AT_7
//  Measure voltage at pH 4.0 buffer → set PH_VOLTAGE_AT_4
//  The library will linearly interpolate between these two points.
// ─────────────────────────────────────────────────────────────────────────────
#define PH_VOLTAGE_AT_7    1.500   // Voltage (V) when probe is in pH 7.0 buffer
#define PH_VOLTAGE_AT_4    2.032   // Voltage (V) when probe is in pH 4.0 buffer

/**
 * Initialize the pH sensor.
 * @return true (always succeeds for analog sensor).
 */
bool ph_init();

/**
 * Read the pH value.
 * @param waterTempC  Current water temperature for compensation (default 25°C).
 * @return true if reading is within valid pH range (0–14).
 */
bool ph_read(float waterTempC = 25.0);

/**
 * Get the pH value from the last reading (0–14).
 */
float ph_getValue();

/**
 * Get the raw voltage from the last reading (useful for calibration).
 */
float ph_getVoltage();

/**
 * Get a human-readable sensor name.
 */
const char* ph_getName();

#endif // SENSOR_PH_H
