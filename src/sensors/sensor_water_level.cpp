/*
 * ============================================================================
 *  sensor_water_level.cpp — Analog Resistive Water Level Sensor Implementation
 * ============================================================================
 */

#include "sensor_water_level.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Internal State
// ─────────────────────────────────────────────────────────────────────────────
static int   _rawValue    = 0;
static float _percent     = 0.0;
static int   _errorCount  = 0;

// Number of consecutive bad reads before flagging an error
static const int ERROR_THRESHOLD = 3;

// Number of samples to average for noise reduction
static const int NUM_SAMPLES = 10;

// ─────────────────────────────────────────────────────────────────────────────
//  Public Implementation
// ─────────────────────────────────────────────────────────────────────────────

bool waterLevel_init() {
    pinMode(WATER_LEVEL_POWER_PIN, OUTPUT);
    digitalWrite(WATER_LEVEL_POWER_PIN, LOW);  // Keep sensor OFF initially
    // No special init needed for analogRead on ESP32-S3
    DBGLN("[WATER_LEVEL] Initialized — Signal: GPIO" + String(WATER_LEVEL_SIGNAL_PIN) +
          ", Power: GPIO" + String(WATER_LEVEL_POWER_PIN));
    return true;
}

bool waterLevel_read() {
    // Power ON the sensor
    digitalWrite(WATER_LEVEL_POWER_PIN, HIGH);
    delay(20);  // Allow sensor to stabilize

    // Take multiple samples and average to reduce noise
    long sum = 0;
    for (int i = 0; i < NUM_SAMPLES; i++) {
        sum += analogRead(WATER_LEVEL_SIGNAL_PIN);
        delayMicroseconds(100);
    }

    // Power OFF the sensor to prevent corrosion
    digitalWrite(WATER_LEVEL_POWER_PIN, LOW);

    _rawValue = (int)(sum / NUM_SAMPLES);

    // Convert to percentage (0–4095 → 0–100%)
    _percent = (_rawValue / 4095.0) * 100.0;

    // Error detection: stuck at extremes for multiple reads
    if (_rawValue <= 5 || _rawValue >= 4090) {
        _errorCount++;
        if (_errorCount >= ERROR_THRESHOLD) {
            DBGF("[WATER_LEVEL] ERROR: Stuck at %d for %d reads\n", _rawValue, _errorCount);
            return false;
        }
    } else {
        _errorCount = 0;  // Reset on good read
    }

    DBGF("[WATER_LEVEL] Raw: %d | Percent: %.1f%%\n", _rawValue, _percent);
    return true;
}

int waterLevel_getRaw() {
    return _rawValue;
}

float waterLevel_getPercent() {
    return _percent;
}

const char* waterLevel_getName() {
    return "Water Level";
}
