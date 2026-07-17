#include "sensors.h"
#include "../core/state.h"
#include <Arduino.h>

// Assuming a standard 12-bit ADC for ESP32 and 3.3V reference
#define VREF 3.3
#define ADC_RES 4095.0

void initTDS() {
    if (currentConfig.pin_tds >= 0) {
        // Only initialize if a valid pin is assigned in Web Doctor settings
        pinMode(currentConfig.pin_tds, INPUT);
        webLog(1, "info", "TDS sensor initialized on pin " + String(currentConfig.pin_tds));
    } else {
        webLog(1, "warn", "TDS sensor disabled (pin set to -1)");
    }
}

float readTDS(float currentWaterTemp) {
    // 1. Guard check: return 0.0 immediately if the sensor pin is undefined/disabled
    if (currentConfig.pin_tds < 0) {
        return 0.0;
    }

    // 2. Read the raw analog value
    int analogValue = analogRead(currentConfig.pin_tds);
    float voltage = analogValue * (VREF / ADC_RES);

    // 3. Apply Temperature Compensation
    // If the water temp sensor is returning 0 or fails, default to standard 25°C
    if (currentWaterTemp <= 0.0) {
        currentWaterTemp = 25.0;
    }
    float compensationCoefficient = 1.0 + 0.02 * (currentWaterTemp - 25.0);
    float compensationVoltage = voltage / compensationCoefficient;

    // 4. Calculate TDS value and apply the live calibration K-factor from NVS
    // Standard gravity formula multiplied by our dynamic offset/multiplier
    float tdsValue = (133.42 * pow(compensationVoltage, 3)
                    - 255.86 * pow(compensationVoltage, 2)
                    + 857.39 * compensationVoltage) * 0.5 * currentConfig.tds_k;

    // 5. Sanity bounds check
    if (tdsValue < 0.0) {
        tdsValue = 0.0;
    }

    return tdsValue;
}
