#include "../core/state.h"
#include <Wire.h>
#include <BH1750.h>

float readLight();

BH1750 lightMeter;
bool isLightSensorReady = false;

// BH1750 default I2C address (ADDR pin low/floating). Used only for the
// bounded presence probe below, before we hand off to the BH1750 library.
#define BH1750_PROBE_ADDR 0x23

void initLight()
{
    // 1. Guard check: if either SDA or SCL is < 0 (e.g., -1), completely disable the sensor
    if (currentConfig.pin_lux_sda < 0 || currentConfig.pin_lux_scl < 0)
    {
        isLightSensorReady = false;
        webLog(1, LOG_WARN, "BH1750 Light sensor disabled (SDA/SCL set to -1)");
        return;
    }

    // Initialize the I2C bus using the dynamic pins from Web Doctor
    Wire.begin(currentConfig.pin_lux_sda, currentConfig.pin_lux_scl);

    // 2. CRITICAL: bound every I2C transaction. Without this, a floating or
    // stuck SDA/SCL line can make the ESP32 Wire driver block forever
    // (no ACK, no clock-stretch timeout), which starves the sensor task
    // and trips the Core 1 task watchdog -> panic -> reboot. 1000ms is
    // generous for a healthy bus and still finite for a broken one.
    Wire.setTimeOut(1000);

    // 3. Non-blocking presence probe BEFORE calling into the BH1750 library.
    // beginTransmission/endTransmission respects the timeout set above, so
    // this can now only ever take up to ~1s, never hang indefinitely.
    Wire.beginTransmission(BH1750_PROBE_ADDR);
    uint8_t probeResult = Wire.endTransmission();

    if (probeResult != 0)
    {
        // 0 = ACK received, sensor is there. Anything else (timeout,
        // NACK, bus error) means nothing responded on that address.
        isLightSensorReady = false;
        webLog(1, LOG_ERR, "BH1750 not detected on I2C (SDA: " + String(currentConfig.pin_lux_sda) + ", SCL: " + String(currentConfig.pin_lux_scl) + "). Check wiring/pull-ups. Sensor disabled for this session.");
        return;
    }

    if (lightMeter.begin())
    {
        isLightSensorReady = true;
        webLog(1, LOG_INFO, "BH1750 Light sensor initialized on I2C (SDA: " + String(currentConfig.pin_lux_sda) + ", SCL: " + String(currentConfig.pin_lux_scl) + ")");
    }
    else
    {
        isLightSensorReady = false;
        webLog(1, LOG_ERR, "Failed to initialize BH1750 Light sensor. Check wiring.");
    }
}

bool sensor_lux_init()
{
    initLight();
    return isLightSensorReady;
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
