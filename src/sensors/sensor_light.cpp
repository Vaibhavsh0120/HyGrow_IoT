/*
 * ============================================================================
 *  sensor_light.cpp — BH1750 Digital Light Sensor Implementation
 * ============================================================================
 */

#include "sensor_light.h"
#include <Wire.h>
#include <BH1750.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Internal State
// ─────────────────────────────────────────────────────────────────────────────
static BH1750 lightSensor;
static float _luxValue = 0.0;
static bool  _initialized = false;

// ─────────────────────────────────────────────────────────────────────────────
//  Public Implementation
// ─────────────────────────────────────────────────────────────────────────────

bool light_init() {
    // Initialize I2C with custom pins for ESP32-S3
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    if (lightSensor.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, BH1750_I2C_ADDR, &Wire)) {
        _initialized = true;
        DBGLN("[LIGHT] BH1750 initialized — I2C SDA: GPIO" + String(I2C_SDA_PIN) +
              ", SCL: GPIO" + String(I2C_SCL_PIN) +
              ", Addr: 0x" + String(BH1750_I2C_ADDR, HEX));
        return true;
    }

    DBGLN("[LIGHT] ERROR: BH1750 not found on I2C bus!");
    DBGLN("[LIGHT] Check wiring: VCC→3.3V, GND→GND, SDA→GPIO" + String(I2C_SDA_PIN) +
          ", SCL→GPIO" + String(I2C_SCL_PIN));
    return false;
}

bool light_read() {
    if (!_initialized) {
        DBGLN("[LIGHT] ERROR: Sensor not initialized");
        return false;
    }

    if (lightSensor.measurementReady()) {
        _luxValue = lightSensor.readLightLevel();

        if (_luxValue < 0) {
            DBGF("[LIGHT] ERROR: Invalid reading (%.1f lx)\n", _luxValue);
            return false;
        }

        DBGF("[LIGHT] Lux: %.1f lx\n", _luxValue);
        return true;
    }

    // Measurement not ready yet — use last value, not an error
    DBGLN("[LIGHT] Measurement not ready, using last value");
    return true;
}

float light_getLux() {
    return _luxValue;
}

const char* light_getName() {
    return "BH1750 Light";
}
