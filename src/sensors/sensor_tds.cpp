#include <Arduino.h>
#include "../../config.h"

static int analogBuffer[30];

void init_tds() {
    analogReadResolution(12);
}

// Median filter (DFRobot's algorithm)
int getMedianNum(int bArray[], int iFilterLen) {
    int bTab[iFilterLen];
    for (int i = 0; i < iFilterLen; i++) bTab[i] = bArray[i];
    for (int j = 0; j < iFilterLen - 1; j++) {
        for (int i = 0; i < iFilterLen - j - 1; i++) {
            if (bTab[i] > bTab[i + 1]) {
                int tmp = bTab[i]; bTab[i] = bTab[i + 1]; bTab[i + 1] = tmp;
            }
        }
    }
    return (iFilterLen & 1) ? bTab[(iFilterLen - 1) / 2] : (bTab[iFilterLen / 2] + bTab[iFilterLen / 2 - 1]) / 2;
}

float read_tds(float water_temp_c) {
    // Read 30 samples fast for filtering
    for(int i=0; i < 30; i++) {
        analogBuffer[i] = analogRead(PIN_TDS_SIG);
        delay(2);
    }
    float medianRaw = getMedianNum(analogBuffer, 30);
    float avgVoltage = medianRaw * (3.3f / 4095.0f);
    float compCoeff = 1.0 + 0.02 * (water_temp_c - 25.0);
    float compVoltage = avgVoltage / compCoeff;

    float tdsValue = (133.42 * pow(compVoltage, 3) - 255.86 * pow(compVoltage, 2) + 857.39 * compVoltage) * 0.5;
    return tdsValue;
}
