#include "../core/state.h"
#include <Arduino.h>

float readPH();

// Assuming a standard 12-bit ADC for ESP32 and 3.3V reference
#define VREF 3.3
#define ADC_RES 4095.0

void initPH()
{
    if (currentConfig.pin_ph >= 0)
    {
        // Only initialize if a valid pin is assigned in Web Doctor settings
        pinMode(currentConfig.pin_ph, INPUT);
        webLog(1, LOG_INFO, "pH sensor initialized on pin " + String(currentConfig.pin_ph));
    }
    else
    {
        webLog(1, LOG_WARN, "pH sensor disabled (pin set to -1)");
    }
}

void sensor_ph_init()
{
    initPH();
}

bool sensor_ph_read(float ph_offset, float ph_slope, float &ph_value)
{
    currentConfig.ph_offset = ph_offset;
    currentConfig.ph_slope = ph_slope;
    float value = readPH();
    ph_value = value;
    return !isnan(value);
}

float readPH()
{
    // 1. Guard check: return NaN immediately if the sensor pin is undefined/disabled,
    // so sensor_ph_read() correctly reports failure instead of a false "ok" at 0.0.
    if (currentConfig.pin_ph < 0)
    {
        return NAN;
    }

    // 2. Read the raw analog value
    // (Note: In future iterations, you may want to add a median filter or multi-sampling here for stability)
    int analogValue = analogRead(currentConfig.pin_ph);
    float voltage = analogValue * (VREF / ADC_RES);

    // 3. Calculate pH value using live calibration variables from NVS
    // Linear equation: pH = (slope * voltage) + offset
    float phValue = (currentConfig.ph_slope * voltage) + currentConfig.ph_offset;

    // 4. Sanity bounds check (pH is strictly 0 to 14)
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
