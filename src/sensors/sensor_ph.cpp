#include "sensors.h"
#include "../core/state.h"
#include <Arduino.h>

// Assuming a standard 12-bit ADC for ESP32 and 3.3V reference
#define VREF 3.3
#define ADC_RES 4095.0

void initPH() {
    if (currentConfig.pin_ph >= 0) {
        // Only initialize if a valid pin is assigned in Web Doctor settings
        pinMode(currentConfig.pin_ph, INPUT);
        webLog(1, "info", "pH sensor initialized on pin " + String(currentConfig.pin_ph));
    } else {
        webLog(1, "warn", "pH sensor disabled (pin set to -1)");
    }
}

float readPH() {
    // 1. Guard check: return 0.0 immediately if the sensor pin is undefined/disabled.
    // The backend network task will see 0.0 and know not to push to Firestore.
    if (currentConfig.pin_ph < 0) {
        return 0.0;
    }

    // 2. Read the raw analog value
    // (Note: In future iterations, you may want to add a median filter or multi-sampling here for stability)
    int analogValue = analogRead(currentConfig.pin_ph);
    float voltage = analogValue * (VREF / ADC_RES);

    // 3. Calculate pH value using live calibration variables from NVS
    // Linear equation: pH = (slope * voltage) + offset
    float phValue = (currentConfig.ph_slope * voltage) + currentConfig.ph_offset;

    // 4. Sanity bounds check (pH is strictly 0 to 14)
    if (phValue < 0.0) {
        phValue = 0.0;
    } else if (phValue > 14.0) {
        phValue = 14.0;
    }

    return phValue;
}
