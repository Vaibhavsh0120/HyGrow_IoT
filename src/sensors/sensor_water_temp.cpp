#include <OneWire.h>
#include <DallasTemperature.h>
#include "../../config.h"

OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);

void init_wtemp() {
    ds18b20.begin();
}

bool read_wtemp(float &temp) {
    ds18b20.requestTemperatures();
    float t = ds18b20.getTempCByIndex(0);
    if (t == DEVICE_DISCONNECTED_C) return false;

    temp = t;
    return true;
}
