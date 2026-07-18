#include "../core/state.h"
#include <Arduino.h>

float readTDS(float currentWaterTemp);

#include <Arduino.h>
#define SCOUNT 30

// Median filter (DFRobot's algorithm)
int getMedianNum(int bArray[], int iFilterLen)
{
    int bTab[iFilterLen];
    for (int i = 0; i < iFilterLen; i++)
        bTab[i] = bArray[i];
    for (int j = 0; j < iFilterLen - 1; j++)
    {
        for (int i = 0; i < iFilterLen - j - 1; i++)
        {
            if (bTab[i] > bTab[i + 1])
            {
                int tmp = bTab[i];
                bTab[i] = bTab[i + 1];
                bTab[i + 1] = tmp;
            }
        }
    }
    return (iFilterLen & 1)
               ? bTab[(iFilterLen - 1) / 2]
               : (bTab[iFilterLen / 2] + bTab[iFilterLen / 2 - 1]) / 2;
}

void initTDS()
{
    if (currentConfig.pin_tds >= 0)
    {
        // Only initialize if a valid pin is assigned in Web Doctor settings
        pinMode(currentConfig.pin_tds, INPUT);
        analogReadResolution(12);
        webLog(1, LOG_INFO, "TDS sensor initialized on pin " + String(currentConfig.pin_tds));
    }
    else
    {
        webLog(1, LOG_WARN, "TDS sensor disabled (pin set to -1)");
    }
}

void sensor_tds_init()
{
    initTDS();
}

bool sensor_tds_read(float water_temp_c, float tds_k, float &tds_ppm)
{
    float prev_k = currentConfig.tds_k;
    currentConfig.tds_k = tds_k;
    float value = readTDS(water_temp_c);
    currentConfig.tds_k = prev_k;
    tds_ppm = value;
    return !isnan(value);
}

float readTDS(float currentWaterTemp)
{
    // 1. Guard check: return NaN immediately if the sensor pin is disabled, so
    // sensor_tds_read() correctly reports failure instead of a false "ok" at 0.0.
    if (currentConfig.pin_tds < 0)
    {
        return NAN;
    }

    // 2. Rapidly sample 30 times into a local buffer
    // Taking samples 2ms apart ensures we get a good average without blocking FreeRTOS too long
    int analogBuffer[SCOUNT];
    for (int i = 0; i < SCOUNT; i++)
    {
        analogBuffer[i] = analogReadMilliVolts(currentConfig.pin_tds);
        delay(2);
    }

    // 3. Apply Median Filter
    int medianRaw = getMedianNum(analogBuffer, SCOUNT);
    float avgVoltage = medianRaw / 1000.0;

    // 4. Temperature Compensation
    if (currentWaterTemp <= 0.0)
    {
        currentWaterTemp = 25.0; // Fallback if water temp sensor fails
    }
    float compCoeff = 1.0 + 0.02 * (currentWaterTemp - 25.0);
    float compVoltage = avgVoltage / compCoeff;

    // 5. Calculate final TDS using polynomial and NVS calibration K-Factor
    float tdsValue = (133.42 * pow(compVoltage, 3) - 255.86 * pow(compVoltage, 2) + 857.39 * compVoltage) * 0.5 * currentConfig.tds_k;

    // 6. Sanity bounds check
    if (tdsValue < 0.0)
    {
        tdsValue = 0.0;
    }

    return tdsValue;
}
