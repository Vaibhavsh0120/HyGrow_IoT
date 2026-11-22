#ifndef TASK_SENSOR_H
#define TASK_SENSOR_H

#include <Arduino.h>

// Compatibility helpers used by the main sketch task wrapper.
void initSensorTask();
void sensorTaskLoop();

#endif // TASK_SENSOR_H
