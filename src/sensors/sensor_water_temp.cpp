#include "../core/state.h"
#include <OneWire.h>
#include <DallasTemperature.h>

float readWaterTemp();

// Dynamically allocate objects so we can assign pins from NVS at runtime
OneWire *oneWire = nullptr;
DallasTemperature *waterTempSensor = nullptr;

void initWaterTemp()
{
    // The pin can be reassigned at runtime from the Web Doctor dashboard,
    // so initWaterTemp() may run more than once per boot. Free any
    // previously allocated instances before replacing the pointers,
    // otherwise every reassignment leaks the old objects. Delete the
    // DallasTemperature instance first since it holds a reference to the
    // OneWire instance.
    if (waterTempSensor != nullptr)
    {
        delete waterTempSensor;
    }
    if (oneWire != nullptr)
    {
        delete oneWire;
    }

    oneWire = new OneWire(currentConfig.pin_ds18b20);
    waterTempSensor = new DallasTemperature(oneWire);
    waterTempSensor->begin();
    webLog(1, LOG_INFO, "DS18B20 initialized on pin " + String(currentConfig.pin_ds18b20));
}

void sensor_ds18b20_init()
{
    initWaterTemp();
}

bool sensor_ds18b20_read(float &temp_c)
{
    float value = readWaterTemp();
    temp_c = value;
    return !isnan(value);
}

float readWaterTemp()
{
    // 1. Guard check: nothing to read from if init() hasn't run yet.
    // sensor_enabled[S_WTEMP] is what actually decides whether this ever
    // gets called in practice — see validateSensor()/readAll() in
    // task_sensor.cpp.
    if (waterTempSensor == nullptr)
    {
        return NAN;
    }

    // 2. Request temperatures on the OneWire bus
    waterTempSensor->requestTemperatures();
    float tempC = waterTempSensor->getTempCByIndex(0);

    // 3. Error Handling
    if (tempC == DEVICE_DISCONNECTED_C)
    {
        webLog(1, LOG_ERR, "DS18B20 sensor disconnected or not found!");
        return NAN;
    }

    return tempC;
}
