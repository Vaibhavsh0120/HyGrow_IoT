/*
 * ============================================================================
 *  sensor_tds.cpp — DFRobot Gravity Analog TDS Sensor Implementation
 * ============================================================================
 *  
 *  Manual voltage-to-TDS conversion formula (from DFRobot documentation):
 *  
 *  1. Read analog voltage
 *  2. Apply temperature compensation coefficient
 *  3. Convert compensated voltage to TDS (ppm) using polynomial formula
 *  
 *  Formula:
 *    compensationCoefficient = 1.0 + 0.02 * (temp - 25.0)
 *    compensationVoltage = voltage / compensationCoefficient
 *    TDS = (133.42 * V^3 - 255.86 * V^2 + 857.39 * V) * 0.5
 *    where V = compensationVoltage
 * ============================================================================
 */

#include "sensor_tds.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Internal State
// ─────────────────────────────────────────────────────────────────────────────
static float _tdsPPM     = 0.0;
static int   _errorCount = 0;

// Number of samples to average for noise reduction
static const int NUM_SAMPLES = 20;

// Error threshold
static const int ERROR_THRESHOLD = 5;
static const float TDS_MAX_VALID = 1500.0;  // ppm — above this likely sensor error

// ─────────────────────────────────────────────────────────────────────────────
//  Private Helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Read analog voltage with averaging.
 */
static float readAverageVoltage() {
    long sum = 0;
    for (int i = 0; i < NUM_SAMPLES; i++) {
        sum += analogRead(TDS_SENSOR_PIN);
        delayMicroseconds(50);
    }
    float avgRaw = (float)sum / NUM_SAMPLES;
    return avgRaw * TDS_AREF_VOLTAGE / (float)TDS_ADC_RANGE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public Implementation
// ─────────────────────────────────────────────────────────────────────────────

bool tds_init() {
    // No special initialization needed for analog read
    DBGLN("[TDS] Initialized — Signal: GPIO" + String(TDS_SENSOR_PIN) +
          ", Vref: " + String(TDS_AREF_VOLTAGE) + "V, ADC: " + String(TDS_ADC_RANGE));
    return true;
}

bool tds_read(float waterTempC) {
    // Step 1: Read average voltage
    float voltage = readAverageVoltage();

    // Step 2: Temperature compensation
    float compensationCoefficient = 1.0 + 0.02 * (waterTempC - 25.0);
    float compensationVoltage = voltage / compensationCoefficient;

    // Step 3: Convert to TDS using DFRobot polynomial formula
    float v = compensationVoltage;
    _tdsPPM = (133.42 * v * v * v - 255.86 * v * v + 857.39 * v) * 0.5;

    // Clamp to zero if negative (noise)
    if (_tdsPPM < 0) _tdsPPM = 0;

    // Error detection
    if (voltage < 0.01 || _tdsPPM > TDS_MAX_VALID) {
        _errorCount++;
        if (_errorCount >= ERROR_THRESHOLD) {
            DBGF("[TDS] ERROR: Voltage=%.3fV, TDS=%.1f ppm (out of range, %d consecutive)\n",
                 voltage, _tdsPPM, _errorCount);
            return false;
        }
    } else {
        _errorCount = 0;
    }

    DBGF("[TDS] Voltage: %.3fV | TDS: %.1f ppm (Temp comp: %.1f°C)\n",
         voltage, _tdsPPM, waterTempC);
    return true;
}

float tds_getPPM() {
    return _tdsPPM;
}

const char* tds_getName() {
    return "TDS Water Quality";
}
