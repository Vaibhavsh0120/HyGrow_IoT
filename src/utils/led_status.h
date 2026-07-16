/*
 * ============================================================================
 *  led_status.h — RGB NeoPixel LED Status Indicator
 * ============================================================================
 *  
 *  Controls the built-in WS2812 NeoPixel on ESP32-S3 (GPIO 48).
 *  Each sensor has a unique error color for quick visual diagnostics.
 *  
 *  Color Map:
 *    Water Level  → Red       (255, 0, 0)
 *    BH1750 Light → Yellow    (255, 255, 0)
 *    TDS          → Purple    (128, 0, 255)
 *    DHT22        → Orange    (255, 80, 0)
 *    pH Sensor    → Blue      (0, 0, 255)
 *    DS18B20      → Cyan      (0, 255, 255)
 *    Firebase     → White     (255, 255, 255)
 *    All OK       → Green     (0, 255, 0)
 * ============================================================================
 */

#ifndef LED_STATUS_H
#define LED_STATUS_H

#include <Arduino.h>
#include "../../config.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Initialize the NeoPixel LED. Call once in setup().
 */
void ledStatusInit();

/**
 * Set the LED to the error color for a specific sensor.
 * @param id  The SensorID whose error color to display.
 */
void ledSetError(SensorID id);

/**
 * Set the LED to green — all sensors reading OK.
 */
void ledSetOK();

/**
 * Set the LED to white blink — Firebase connectivity error.
 */
void ledSetFirebaseError();

/**
 * Turn off the LED completely.
 */
void ledClear();

/**
 * Cycle through all active errors (call in loop for multi-error indication).
 * Pass an array of booleans indicating which sensors have errors.
 * @param errors  Array of SENSOR_COUNT booleans.
 */
void ledCycleErrors(bool errors[]);

/**
 * Show a startup animation (color sweep).
 */
void ledStartupAnimation();

#endif // LED_STATUS_H
