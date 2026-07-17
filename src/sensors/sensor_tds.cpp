#include "sensors.h"
#include "../core/state.h"
#include <Arduino.h>

#define VREF         3.3f
#define ADC_RES      4095.0f
#define SCOUNT       30

// Median filter (DFRobot's algorithm)
int getMedianNum(int bArray[], int iFilterLen) {
    int bTab[iFilterLen];
    for (int i = 0; i < iFilterLen; i++) bTab[i] = bArray[i];
    for (int j = 0; j < iFilterLen - 1; j++) {
        for (int i = 0; i < iFilterLen - j - 1; i++) {
            if (bTab[i] > bTab[i + 1]) {
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

void initTDS() {
    if (currentConfig.pin_tds >= 0) {
        // Only initialize if a valid pin is assigned in Web Doctor settings
        pinMode(currentConfig.pin_tds, INPUT);
        analogReadResolution(12);
        webLog(1, "info", "TDS sensor initialized on pin " + String(currentConfig.pin_tds));
    } else {
        webLog(1, "warn", "TDS sensor disabled (pin set to -1)");
    }
}

float readTDS(float currentWaterTemp) {
    // 1. Guard check: return 0.0 immediately if the sensor pin is disabled
    if (currentConfig.pin_tds < 0) {
        return 0.0;
    }

    // 2. Rapidly sample 30 times into a local buffer
    // Taking samples 2ms apart ensures we get a good average without blocking FreeRTOS too long
    int analogBuffer[SCOUNT];
    for(int i = 0; i < SCOUNT; i++) {
        analogBuffer[i] = analogRead(currentConfig.pin_tds);
        delay(2);
    }

    // 3. Apply Median Filter
    int medianRaw = getMedianNum(analogBuffer, SCOUNT);
    float avgVoltage = medianRaw * (VREF / ADC_RES);

    // 4. Temperature Compensation
    if (currentWaterTemp <= 0.0) {
        currentWaterTemp = 25.0; // Fallback if water temp sensor fails
    }
    float compCoeff = 1.0 + 0.02 * (currentWaterTemp - 25.0);
    float compVoltage = avgVoltage / compCoeff;

    // 5. Calculate final TDS using polynomial and NVS calibration K-Factor
    float tdsValue = (133.42 * pow(compVoltage, 3)
                    - 255.86 * pow(compVoltage, 2)
                    + 857.39 * compVoltage) * 0.5 * currentConfig.tds_k;

    // 6. Sanity bounds check
    if (tdsValue < 0.0) {
        tdsValue = 0.0;
    }

    return tdsValue;
}
