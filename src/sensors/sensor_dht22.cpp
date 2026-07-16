#include <DHT.h>
#include "../../config.h"

DHT dht(PIN_DHT22, DHT22);

void init_dht() {
    dht.begin();
}

bool read_dht(float &temp, float &hum) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (isnan(t) || isnan(h)) return false;

    temp = t;
    hum = h;
    return true;
}
