/*
 * ============================================================================
 *  sensor_dht22.cpp — DHT22 / AM2302 Sensor Implementation
 * ============================================================================
 */

#include "sensor_dht22.h"
#include <DHT.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Internal State
// ─────────────────────────────────────────────────────────────────────────────
static DHT dht(DHT22_DATA_PIN, DHT22);

static float _temperature = 0.0;
static float _humidity    = 0.0;
static int   _errorCount  = 0;

static const int ERROR_THRESHOLD = 3;

// ─────────────────────────────────────────────────────────────────────────────
//  Public Implementation
// ─────────────────────────────────────────────────────────────────────────────

bool dht22_init() {
    dht.begin();
    DBGLN("[DHT22] Initialized — Data: GPIO" + String(DHT22_DATA_PIN));
    
    // Small delay for sensor warm-up
    delay(2000);

    // Try an initial read to verify connection
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (isnan(t) || isnan(h)) {
        DBGLN("[DHT22] WARNING: Initial read failed — sensor may need more time to warm up");
        // Don't return false here — DHT22 sometimes needs a couple attempts
    } else {
        DBGF("[DHT22] Initial read OK — Temp: %.1f°C, Humidity: %.1f%%\n", t, h);
    }

    return true;
}

bool dht22_read() {
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    // Validate readings
    if (isnan(t) || isnan(h)) {
        _errorCount++;
        if (_errorCount >= ERROR_THRESHOLD) {
            DBGF("[DHT22] ERROR: NaN readings for %d consecutive attempts\n", _errorCount);
            DBGLN("[DHT22] Check wiring: VCC→3.3V, GND→GND, Data→GPIO" + String(DHT22_DATA_PIN));
            return false;
        }
        DBGF("[DHT22] WARNING: NaN reading (attempt %d/%d)\n", _errorCount, ERROR_THRESHOLD);
        return true;  // Use last valid value, not a hard error yet
    }

    // Sanity check — DHT22 range: -40 to 80°C, 0–100% humidity
    if (t < -40.0 || t > 80.0 || h < 0.0 || h > 100.0) {
        _errorCount++;
        DBGF("[DHT22] WARNING: Implausible values — Temp: %.1f°C, Humidity: %.1f%%\n", t, h);
        if (_errorCount >= ERROR_THRESHOLD) {
            return false;
        }
        return true;
    }

    // Good reading
    _temperature = t;
    _humidity = h;
    _errorCount = 0;

    DBGF("[DHT22] Temp: %.1f°C | Humidity: %.1f%%\n", _temperature, _humidity);
    return true;
}

float dht22_getTemperature() {
    return _temperature;
}

float dht22_getHumidity() {
    return _humidity;
}

const char* dht22_getName() {
    return "DHT22 Air Temp/Humidity";
}
