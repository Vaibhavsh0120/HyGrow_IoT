/*
 * ============================================================================
 *  sensor_ph.cpp — DFRobot Gravity Lab pH Sensor V2 Implementation
 * ============================================================================
 *  
 *  Two-point linear calibration:
 *  
 *  slope = (7.0 - 4.0) / (V@7 - V@4)
 *  pH = 7.0 + slope * (voltage - V@7)
 *  
 *  Since higher pH → lower voltage (typically), the slope is negative.
 *  Adjust PH_VOLTAGE_AT_7 and PH_VOLTAGE_AT_4 in sensor_ph.h after
 *  calibrating with standard buffer solutions.
 * ============================================================================
 */

#include "sensor_ph.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Internal State
// ─────────────────────────────────────────────────────────────────────────────
static float _phValue    = 7.0;
static float _voltage    = 0.0;
static int   _errorCount = 0;

// Number of samples to average
static const int NUM_SAMPLES = 20;

// Error threshold
static const int ERROR_THRESHOLD = 5;

// ─────────────────────────────────────────────────────────────────────────────
//  Private Helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Read analog voltage with median filtering for noise rejection.
 * Takes NUM_SAMPLES readings, sorts them, and returns the median.
 */
static float readFilteredVoltage() {
    int readings[NUM_SAMPLES];

    for (int i = 0; i < NUM_SAMPLES; i++) {
        readings[i] = analogRead(PH_SENSOR_PIN);
        delayMicroseconds(50);
    }

    // Simple bubble sort for median extraction
    for (int i = 0; i < NUM_SAMPLES - 1; i++) {
        for (int j = 0; j < NUM_SAMPLES - i - 1; j++) {
            if (readings[j] > readings[j + 1]) {
                int temp = readings[j];
                readings[j] = readings[j + 1];
                readings[j + 1] = temp;
            }
        }
    }

    // Take median (middle 6 values averaged for extra smoothness)
    long sum = 0;
    int start = NUM_SAMPLES / 2 - 3;
    int end = NUM_SAMPLES / 2 + 3;
    if (start < 0) start = 0;
    if (end > NUM_SAMPLES) end = NUM_SAMPLES;

    for (int i = start; i < end; i++) {
        sum += readings[i];
    }

    float avgRaw = (float)sum / (end - start);
    return avgRaw * PH_AREF_VOLTAGE / (float)PH_ADC_RANGE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public Implementation
// ─────────────────────────────────────────────────────────────────────────────

bool ph_init() {
    // No special initialization for analog read
    float slope = (7.0 - 4.0) / (PH_VOLTAGE_AT_7 - PH_VOLTAGE_AT_4);

    DBGLN("[PH] Initialized — Signal: GPIO" + String(PH_SENSOR_PIN));
    DBGF("[PH] Calibration: V@7=%.3fV, V@4=%.3fV, Slope=%.3f\n",
         PH_VOLTAGE_AT_7, PH_VOLTAGE_AT_4, slope);
    return true;
}

bool ph_read(float waterTempC) {
    // Step 1: Read filtered voltage
    _voltage = readFilteredVoltage();

    // Step 2: Two-point linear interpolation
    float slope = (7.0 - 4.0) / (PH_VOLTAGE_AT_7 - PH_VOLTAGE_AT_4);
    _phValue = 7.0 + slope * (_voltage - PH_VOLTAGE_AT_7);

    // Step 3: Temperature compensation (Nernst equation simplification)
    // The theoretical slope changes by ~0.003 pH/°C
    // This is a simplified correction; for lab-grade accuracy, use proper Nernst
    float tempCompensation = 0.003 * (waterTempC - 25.0);
    _phValue += tempCompensation;

    // Step 4: Validate
    if (_voltage < 0.05) {
        _errorCount++;
        if (_errorCount >= ERROR_THRESHOLD) {
            DBGF("[PH] ERROR: No voltage detected (%.3fV) — sensor disconnected?\n", _voltage);
            return false;
        }
    } else if (_phValue < -0.5 || _phValue > 14.5) {
        _errorCount++;
        if (_errorCount >= ERROR_THRESHOLD) {
            DBGF("[PH] ERROR: pH=%.2f out of range (V=%.3fV)\n", _phValue, _voltage);
            return false;
        }
    } else {
        _errorCount = 0;
    }

    // Clamp to valid range for display
    if (_phValue < 0) _phValue = 0;
    if (_phValue > 14) _phValue = 14;

    DBGF("[PH] Voltage: %.3fV | pH: %.2f (Temp comp: %.1f°C)\n",
         _voltage, _phValue, waterTempC);
    return true;
}

float ph_getValue() {
    return _phValue;
}

float ph_getVoltage() {
    return _voltage;
}

const char* ph_getName() {
    return "pH Sensor";
}
