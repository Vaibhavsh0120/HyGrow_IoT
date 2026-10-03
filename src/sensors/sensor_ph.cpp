#include "../core/state.h"
#include <Arduino.h>

static float readPH(float offset, float slope);

// Assuming a standard 12-bit ADC for ESP32 and 3.3V reference

void initPH()
{
    pinMode(currentConfig.pin_ph, INPUT);
    webLog(1, LOG_INFO, "pH sensor initialized on pin " + String(currentConfig.pin_ph));
}

void sensor_ph_init()
{
    initPH();
}

bool sensor_ph_read(float ph_offset, float ph_slope, float &ph_value)
{
    float value = readPH(ph_offset, ph_slope);
    ph_value = value;
    return !isnan(value);
}

static float readPH(float offset, float slope)
{
    // 1. Read the raw analog value in true Volts using hardware calibration.
    // sensor_enabled[S_PH] is what decides whether this ever gets called in
    // practice — see validateSensor()/readAll() in task_sensor.cpp.
    const int pin = currentConfig.pin_ph;
    const float milliVolts = analogReadMilliVolts(pin);
    currentSensors.ph_voltage_mv = milliVolts;
    float voltage = milliVolts / 1000.0f;

    // 2. Calculate pH value using live calibration variables from NVS
    // Linear equation: pH = (slope * voltage) + offset
    float phValue = (slope * voltage) + offset;

    // 3. Sanity bounds check (pH is strictly 0 to 14)
    if (phValue < 0.0)
    {
        phValue = 0.0;
    }
    else if (phValue > 14.0)
    {
        phValue = 14.0;
    }

    return phValue;
}
