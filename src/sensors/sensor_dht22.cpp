#include "sensors.h"
#include "../core/state.h"
#include <DHT.h>
#include <math.h>

// Dynamically allocate the DHT object so we can use the Web Doctor pin
DHT* dhtSensor = nullptr;

void initDHT22() {
    if (currentConfig.pin_dht >= 0) {
        dhtSensor = new DHT(currentConfig.pin_dht, DHT22);
        dhtSensor->begin();
        webLog(1, "info", "DHT22 initialized on pin " + String(currentConfig.pin_dht));
    } else {
        webLog(1, "warn", "DHT22 sensor disabled (pin set to -1)");
    }
}

void readDHT22(float &outTemp, float &outHum, float &outVpd) {
    // 1. Guard check: Do nothing if the sensor is disabled in Web Doctor
    if (currentConfig.pin_dht < 0 || dhtSensor == nullptr) {
        outTemp = 0.0;
        outHum = 0.0;
        outVpd = 0.0;
        return;
    }

    // 2. Read standard values
    float temp = dhtSensor->readTemperature();
    float hum = dhtSensor->readHumidity();

    // 3. Error Handling
    if (isnan(temp) || isnan(hum)) {
        webLog(1, "error", "Failed to read from DHT22!");
        // We return early here so we don't overwrite the last known good values
        // with 0.0, which would cause ugly spikes on the UI charts.
        return;
    }

    // 4. Update the passed references
    outTemp = temp;
    outHum = hum;

    // 5. Calculate Vapor Pressure Deficit (VPD) in kPa
    // Equation: SVP = 0.61078 * exp((17.27 * T) / (T + 237.3))
    //           VPD = SVP * (1 - (RH / 100))
    float svp = 0.61078 * exp((17.27 * temp) / (temp + 237.3));
    outVpd = svp * (1.0 - (hum / 100.0));

    // Sanity check to prevent negative VPD values
    if (outVpd < 0.0) {
        outVpd = 0.0;
    }
}
