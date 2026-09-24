#ifndef HYGROW_PIN_SAFETY_H
#define HYGROW_PIN_SAFETY_H

#include "../../config.h"

// This firmware targets the ESP32-S3 N16R8 board with the status LED on
// GPIO48. Flash/PSRAM, native USB, and boot strapping pins are not available
// for sensors. The three analog signals use ADC1 while Wi-Fi is active.
inline bool sensorPinIsUsable(int pin, bool analogInput)
{
    if (pin == DEMO_MODE_PIN)
        return true;
    if (pin < 1 || pin > 47 || (pin >= 22 && pin <= 37))
        return false;
    if (pin == 3 || pin == 19 || pin == 20 || pin == 45 || pin == 46)
        return false;
    return !analogInput || pin <= 10;
}

#endif
