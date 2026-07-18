#include "../core/state.h"
#include <Wire.h>
#include <BH1750.h>

float readLight();

BH1750 lightMeter;
bool isLightSensorReady = false;

// I2C bus ownership — Wire.begin(), the transaction timeout, and the BH1750
// presence probe — now lives in task_sensor.cpp, the sole owner of the I2C
// bus on Core 1. By the time this runs, Wire has already been brought up
// and ACK-probed for a BH1750 at this address, so all that's left to do
// here is hand off to the library itself.
bool initLight()
{
    isLightSensorReady = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);
    return isLightSensorReady;
}

bool sensor_lux_init()
{
    bool ok = initLight();
    if (ok)
    {
        webLog(1, LOG_INFO, "BH1750 Light sensor initialized on I2C (SDA: " + String(currentConfig.pin_lux_sda) + ", SCL: " + String(currentConfig.pin_lux_scl) + ")");
    }
    else
    {
        webLog(1, LOG_ERR, "Failed to initialize BH1750 Light sensor. Check wiring.");
    }
    return ok;
}

bool sensor_lux_read(float &lux)
{
    float value = readLight();
    lux = value;
    return !isnan(value);
}

float readLight()
{
    // 1. Guard check: return NaN immediately if initialization failed or sensor is disabled,
    // so sensor_lux_read() correctly reports failure instead of a false "ok" at 0.0.
    if (!isLightSensorReady)
    {
        return NAN;
    }

    // 2. Read the light level in lux
    float lux = lightMeter.readLightLevel();

    // 3. Error Handling
    if (lux < 0.0)
    {
        webLog(1, LOG_ERR, "Error reading from BH1750!");
        return NAN;
    }

    return lux;
}
