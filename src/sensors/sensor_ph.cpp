#include "../core/state.h"
#include <Arduino.h>

float readPH();

// Assuming a standard 12-bit ADC for ESP32 and 3.3V reference
#include <Arduino.h>

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
    // NOTE: ph_offset/ph_slope are accepted as parameters to match the other
    // sensor_*_read() signatures' style, but readPH() below reads
    // currentConfig.ph_offset/ph_slope directly — and the call site
    // (task_sensor.cpp) always passes those exact same fields back in, so
    // there's nothing to apply here. (This used to write the parameters back
    // into currentConfig.ph_offset/ph_slope, which was a pure self-assignment
    // no-op every single read — removed.)
    float value = readPH();
    ph_value = value;
    return !isnan(value);
}

float readPH()
{
    // 1. Read the raw analog value in true Volts using hardware calibration.
    // sensor_enabled[S_PH] is what decides whether this ever gets called in
    // practice — see validateSensor()/readAll() in task_sensor.cpp.
    float voltage = analogReadMilliVolts(currentConfig.pin_ph) / 1000.0;

    // 2. Calculate pH value using live calibration variables from NVS
    // Linear equation: pH = (slope * voltage) + offset
    float phValue = (currentConfig.ph_slope * voltage) + currentConfig.ph_offset;

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
