#include "../core/state.h"
#include <Arduino.h>
#include "analog_input.h"

static float readTDS(uint8_t pin, float currentWaterTemp, float tds_k);

#define SCOUNT 30

// Median filter (DFRobot's algorithm)
int getMedianNum(const int bArray[], int iFilterLen)
{
    int bTab[SCOUNT] = {};
    int len = min(iFilterLen, SCOUNT);
    if (len <= 0)
        return 0;
    for (int i = 0; i < len; i++)
        bTab[i] = bArray[i];
    for (int j = 0; j < len - 1; j++)
    {
        for (int i = 0; i < len - j - 1; i++)
        {
            if (bTab[i] > bTab[i + 1])
            {
                int tmp = bTab[i];
                bTab[i] = bTab[i + 1];
                bTab[i + 1] = tmp;
            }
        }
    }
    return (len & 1)
               ? bTab[(len - 1) / 2]
               : (bTab[len / 2] + bTab[len / 2 - 1]) / 2;
}

void initTDS()
{
    pinMode(currentConfig.pin_tds, INPUT);
    analogReadResolution(12);
    webLog(1, LOG_INFO, "TDS sensor initialized on pin " + String(currentConfig.pin_tds));
}

void sensor_tds_init()
{
    initTDS();
}

bool sensor_tds_read(float water_temp_c, float tds_k, float &tds_ppm)
{
    const uint8_t pin = currentConfig.pin_tds;
    if (!analog_input::isDriven(pin))
    {
        tds_ppm = NAN;
        return false;
    }
    tds_ppm = readTDS(pin, water_temp_c, tds_k);
    return isfinite(tds_ppm);
}

static float readTDS(uint8_t pin, float currentWaterTemp, float tds_k)
{
    // 1. Rapidly sample 30 times into a local buffer. Taking samples 2ms
    // apart ensures we get a good average without blocking FreeRTOS too
    // long. sensor_enabled[S_TDS] is what decides whether this ever gets
    // called in practice — see validateSensor()/readAll() in task_sensor.cpp.
    int analogBuffer[SCOUNT];
    for (int i = 0; i < SCOUNT; i++)
    {
        analogBuffer[i] = analogReadMilliVolts(pin);
        delay(2);
    }

    // 2. Apply Median Filter
    int medianRaw = getMedianNum(analogBuffer, SCOUNT);
    float avgVoltage = medianRaw / 1000.0;

    // 3. Temperature Compensation
    if (currentWaterTemp <= 0.0)
    {
        currentWaterTemp = 25.0; // Fallback if water temp sensor fails
    }
    float compCoeff = 1.0 + 0.02 * (currentWaterTemp - 25.0);
    float compVoltage = avgVoltage / compCoeff;

    // 4. Calculate final TDS using polynomial and NVS calibration K-Factor
    float tdsValue = (133.42 * pow(compVoltage, 3) - 255.86 * pow(compVoltage, 2) + 857.39 * compVoltage) * 0.5 * tds_k;

    // 5. Sanity bounds check
    if (tdsValue < 0.0)
    {
        tdsValue = 0.0;
    }

    return tdsValue;
}
