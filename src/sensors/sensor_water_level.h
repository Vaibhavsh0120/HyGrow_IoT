/*
 * ============================================================================
 *  sensor_water_level.h — Analog Resistive Water Level Sensor
 * ============================================================================
 *  
 *  Sensor: Analog water level detection module
 *  Interface: Analog read (ADC1 pin)
 *  Error Color: 🔴 Red
 *  
 *  Power is toggled via a digital GPIO to reduce corrosion of the
 *  copper traces on the sensor PCB. The sensor is only powered ON
 *  during the brief reading window.
 * ============================================================================
 */

#ifndef SENSOR_WATER_LEVEL_H
#define SENSOR_WATER_LEVEL_H

#include <Arduino.h>
#include "../../config.h"

/**
 * Initialize the water level sensor pins.
 * @return true if initialization succeeds.
 */
bool waterLevel_init();

/**
 * Read the water level sensor.
 * Powers ON the sensor, waits for stabilization, reads, then powers OFF.
 * @return true if reading is valid.
 */
bool waterLevel_read();

/**
 * Get the raw ADC value from the last reading (0–4095).
 */
int waterLevel_getRaw();

/**
 * Get the water level as a percentage (0–100%).
 */
float waterLevel_getPercent();

/**
 * Get a human-readable sensor name.
 */
const char* waterLevel_getName();

#endif // SENSOR_WATER_LEVEL_H
