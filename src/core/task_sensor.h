#ifndef TASK_SENSOR_H
#define TASK_SENSOR_H

#include <Arduino.h>

// Starts the FreeRTOS sensor task pinned to Core 1.
// Reads every currentConfig.interval_read_ms, applies calibration,
// updates currentSensors, and stamps last_ok_ms / last_err per sensor.
void sensor_task_start();

// One-shot: force an immediate read cycle (used after calibration save).
void sensor_task_kick();

#endif // TASK_SENSOR_H
