#include "sensors.h"
#include "../core/state.h"
#include <OneWire.h>
#include <DallasTemperature.h>

// Dynamically allocate objects so we can assign pins from NVS at runtime
OneWire* oneWire = nullptr;
DallasTemperature* waterTempSensor = nullptr;

void initWaterTemp() {
    if (currentConfig.pin_wt >= 0) {
        oneWire = new OneWire(currentConfig.pin_wt);
        waterTempSensor = new DallasTemperature(oneWire);
        waterTempSensor->begin();
        webLog(1, "info", "DS18B20 initialized on pin " + String(currentConfig.pin_wt));
    } else {
        webLog(1, "warn", "DS18B20 sensor disabled (pin set to -1)");
    }
}

float readWaterTemp() {
    // 1. Guard check: return 0.0 immediately if the sensor pin is disabled
    if (currentConfig.pin_wt < 0 || waterTempSensor == nullptr) {
        return 0.0;
    }

    // 2. Request temperatures on the OneWire bus
    waterTempSensor->requestTemperatures();
    float tempC = waterTempSensor->getTempCByIndex(0);

    // 3. Error Handling
    if (tempC == DEVICE_DISCONNECTED_C) {
        webLog(1, "error", "DS18B20 sensor disconnected or not found!");
        // Returning 0.0 signals the backend/UI that the read failed
        return 0.0;
    }

    return tempC;
}
