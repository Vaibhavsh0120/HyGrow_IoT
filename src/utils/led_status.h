#pragma once
#include <Arduino.h>

void ledStatusInit();
void ledSetSolid(uint8_t r, uint8_t g, uint8_t b);
void ledCycleErrors(const bool sensorErrors[], const bool sensorEnabled[]);
