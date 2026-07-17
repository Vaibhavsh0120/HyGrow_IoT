#include "sensors.h"
#include "../core/state.h"
#include <OneWire.h>
#include <DallasTemperature.h>

float readWaterTemp();

// Dynamically allocate objects so we can assign pins from NVS at runtime
OneWire *oneWire = nullptr;
DallasTemperature *waterTempSensor = nullptr;

void initWaterTemp()
{
    if (currentConfig.pin_ds18b20 >= 0)
    {
        oneWire = new OneWire(currentConfig.pin_ds18b20);
        waterTempSensor = new DallasTemperature(oneWire);
        waterTempSensor->begin();
        webLog(1, LOG_INFO, "DS18B20 initialized on pin " + String(currentConfig.pin_ds18b20));
    }
    else
    {
        webLog(1, LOG_WARN, "DS18B20 sensor disabled (pin set to -1)");
    }
}

void init_wtemp()
{
    initWaterTemp();
}

bool read_wtemp(float &temp)
{
    float value = readWaterTemp();
    temp = value;
    return !isnan(value);
}

bool sensor_ds18b20_read(float &temp_c)
{
    return read_wtemp(temp_c);
}

float readWaterTemp()
{
    // 1. Guard check: return 0.0 immediately if the sensor pin is disabled
    if (currentConfig.pin_ds18b20 < 0 || waterTempSensor == nullptr)
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
