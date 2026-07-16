#include <Wire.h>
#include <BH1750.h>
#include "../../config.h"

BH1750 lightMeter;

void init_light() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    lightMeter.begin();
}

float read_light() {
    return lightMeter.readLightLevel(); // Returns -1 or 0 on error
}
