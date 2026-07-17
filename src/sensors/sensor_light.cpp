#include "sensors.h"
#include "../core/state.h"
#include <Wire.h>
#include <BH1750.h>

BH1750 lightMeter;
bool isLightSensorReady = false;

void initLight() {
    // Initialize the I2C bus using the pins from your hardware setup
    // SDA = GPIO 8, SCL = GPIO 9
    Wire.begin(8, 9);

    // Attempt to initialize the BH1750 sensor
    if (lightMeter.begin()) {
        isLightSensorReady = true;
        webLog(1, "info", "BH1750 Light sensor initialized on I2C (SDA: 8, SCL: 9)");
    } else {
        isLightSensorReady = false;
        webLog(1, "error", "Failed to initialize BH1750 Light sensor. Check wiring.");
    }
}

float readLight() {
    // 1. Guard check: return 0.0 immediately if initialization failed
    if (!isLightSensorReady) {
        return 0.0;
    }

    // 2. Read the light level in lux
    float lux = lightMeter.readLightLevel();

    // 3. Error Handling
    // The BH1750 library typically returns a negative number on read failure
    if (lux < 0.0) {
        webLog(1, "error", "Error reading from BH1750!");
        return 0.0;
    }

    return lux;
}
