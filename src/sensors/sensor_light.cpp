#include "sensors.h"
#include "../core/state.h"
#include <Wire.h>
#include <BH1750.h>

BH1750 lightMeter;
bool isLightSensorReady = false;

void initLight() {
    // 1. Guard check: if either SDA or SCL is < 0 (e.g., -1), completely disable the sensor
    if (currentConfig.pin_lux_sda < 0 || currentConfig.pin_lux_scl < 0) {
        isLightSensorReady = false;
        webLog(1, LOG_WARN, "BH1750 Light sensor disabled (SDA/SCL set to -1)");
        return;
    }

    // Initialize the I2C bus using the dynamic pins from Web Doctor
    Wire.begin(currentConfig.pin_lux_sda, currentConfig.pin_lux_scl);

    if (lightMeter.begin()) {
        isLightSensorReady = true;
        webLog(1, LOG_INFO, "BH1750 Light sensor initialized on I2C (SDA: " + String(currentConfig.pin_lux_sda) + ", SCL: " + String(currentConfig.pin_lux_scl) + ")");
    } else {
        isLightSensorReady = false;
        webLog(1, LOG_ERR, "Failed to initialize BH1750 Light sensor. Check wiring.");
    }
}

float readLight() {
    // 1. Guard check: return 0.0 immediately if initialization failed or sensor is disabled
    if (!isLightSensorReady) {
        return 0.0;
    }

    // 2. Read the light level in lux
    float lux = lightMeter.readLightLevel();

    // 3. Error Handling
    if (lux < 0.0) {
        webLog(1, LOG_ERR, "Error reading from BH1750!");
        return 0.0;
    }

    return lux;
}
