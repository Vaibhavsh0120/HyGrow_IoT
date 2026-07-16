/*
 * ============================================================================
 * task_sensor.h — Core 1 Task Declaration
 * ============================================================================
 */
#pragma once

// The FreeRTOS task loop for hardware reads (Runs on Core 1)
void sensor_task_loop(void *pvParameters);
