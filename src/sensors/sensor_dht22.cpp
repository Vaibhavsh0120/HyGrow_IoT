#include "../core/state.h"
#include <DHT.h>
#include <math.h>

void readDHT22(float &outTemp, float &outHum, float &outVpd);

// Dynamically allocate the DHT object so we can use the Web Doctor pin
DHT *dhtSensor = nullptr;

void initDHT22()
{
    if (currentConfig.pin_dht >= 0)
    {
        // The pin can be reassigned at runtime from the Web Doctor dashboard,
        // so initDHT22() may run more than once per boot. Free any previously
        // allocated sensor before replacing the pointer, otherwise every
        // reassignment leaks the old DHT instance.
        if (dhtSensor != nullptr)
        {
            delete dhtSensor;
        }

        dhtSensor = new DHT(currentConfig.pin_dht, DHT22);
        dhtSensor->begin();
        webLog(1, LOG_INFO, "DHT22 initialized on pin " + String(currentConfig.pin_dht));
    }
    else
    {
        webLog(1, LOG_WARN, "DHT22 sensor disabled (pin set to -1)");
    }
}

void sensor_dht_init()
{
    initDHT22();
}

bool sensor_dht_read(float &temp_c, float &humidity_pct)
{
    float vpd = 0.0f;
    readDHT22(temp_c, humidity_pct, vpd);
    return !isnan(temp_c) && !isnan(humidity_pct);
}

void readDHT22(float &outTemp, float &outHum, float &outVpd)
{
    // 1. Guard check: Do nothing if the sensor is disabled in Web Doctor
    if (currentConfig.pin_dht < 0 || dhtSensor == nullptr)
    {
        outTemp = NAN;
        outHum = NAN;
        outVpd = NAN;
        return;
    }

    // 2. Read standard values
    float temp = dhtSensor->readTemperature();
    float hum = dhtSensor->readHumidity();

    // 3. Error Handling
    if (isnan(temp) || isnan(hum))
    {
        webLog(1, LOG_ERR, "Failed to read from DHT22!");
        // Every other sensor driver in this codebase (TDS, pH, light, water
        // level, DS18B20) unconditionally writes its out-param before
        // returning, so sensor_*_read() correctly reports failure via
        // isnan() regardless of what the caller passed in. This function
        // used to be the one exception — it returned early here without
        // touching outTemp/outHum, silently relying on the caller having
        // pre-seeded them with NAN. That happened to hold for readAll()'s
        // call site (task_sensor.cpp seeds `float t = NAN, h = NAN;`) but
        // NOT for validateSensor()'s startup-validation lambda (`float t,
        // h;` — uninitialized stack memory, essentially never NaN by
        // chance). A failed boot-time DHT22 read could therefore be
        // misread as a pass, defeating the 5-attempt startup validation
        // and auto-disable safety net for this one sensor. Explicitly
        // NaN both out-params here so the contract holds no matter what
        // the caller passed in.
        outTemp = NAN;
        outHum = NAN;
        outVpd = NAN;
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
    if (outVpd < 0.0)
    {
        outVpd = 0.0;
    }
}
