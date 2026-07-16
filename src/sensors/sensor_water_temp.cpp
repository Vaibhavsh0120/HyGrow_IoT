/*
 * ============================================================================
 *  sensor_water_temp.cpp — DS18B20 Waterproof Temperature Sensor Implementation
 * ============================================================================
 */

#include "sensor_water_temp.h"
#include <OneWire.h>
#include <DallasTemperature.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Internal State
// ─────────────────────────────────────────────────────────────────────────────
static OneWire           oneWire(DS18B20_DATA_PIN);
static DallasTemperature ds18b20(&oneWire);

static float _temperature   = 0.0;
static int   _deviceCount   = 0;
static int   _errorCount    = 0;

static const int ERROR_THRESHOLD = 3;
static const float DISCONNECTED_TEMP = -127.0;  // DallasTemperature disconnect value

// ─────────────────────────────────────────────────────────────────────────────
//  Public Implementation
// ─────────────────────────────────────────────────────────────────────────────

bool waterTemp_init() {
    ds18b20.begin();
    _deviceCount = ds18b20.getDeviceCount();

    DBGF("[WATER_TEMP] DS18B20 initialized — Data: GPIO%d, Devices found: %d\n",
         DS18B20_DATA_PIN, _deviceCount);

    if (_deviceCount == 0) {
        DBGLN("[WATER_TEMP] ERROR: No DS18B20 devices found!");
        DBGLN("[WATER_TEMP] Check: Data→GPIO" + String(DS18B20_DATA_PIN) +
              " with 4.7kΩ pull-up to 3.3V");
        return false;
    }

    // Set resolution to 12-bit for maximum accuracy (750ms conversion time)
    // Since we read every 2s, this is fine
    ds18b20.setResolution(12);

    // Request initial temperature (non-blocking)
    ds18b20.setWaitForConversion(false);
    ds18b20.requestTemperatures();

    return true;
}

bool waterTemp_read() {
    // Read temperature from the first device (index 0)
    float temp = ds18b20.getTempCByIndex(0);

    // Request next conversion (non-blocking) immediately after reading
    ds18b20.requestTemperatures();

    // Check for disconnect
    if (temp <= DISCONNECTED_TEMP) {
        _errorCount++;
        if (_errorCount >= ERROR_THRESHOLD) {
            DBGF("[WATER_TEMP] ERROR: Disconnected (%.1f°C) for %d reads\n",
                 temp, _errorCount);
            DBGLN("[WATER_TEMP] Check probe connection and 4.7kΩ pull-up resistor");
            return false;
        }
        DBGF("[WATER_TEMP] WARNING: Bad read (%.1f°C), attempt %d/%d\n",
             temp, _errorCount, ERROR_THRESHOLD);
        return true;  // Use last valid value
    }

    // Sanity check — DS18B20 range: -55°C to +125°C
    if (temp < -55.0 || temp > 125.0) {
        _errorCount++;
        DBGF("[WATER_TEMP] WARNING: Implausible temperature: %.1f°C\n", temp);
        if (_errorCount >= ERROR_THRESHOLD) {
            return false;
        }
        return true;
    }

    // Good reading
    _temperature = temp;
    _errorCount = 0;

    DBGF("[WATER_TEMP] Water temp: %.2f°C\n", _temperature);
    return true;
}

float waterTemp_getTemperature() {
    return _temperature;
}

const char* waterTemp_getName() {
    return "DS18B20 Water Temp";
}
