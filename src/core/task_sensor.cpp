/*
 * ============================================================================
 * task_sensor.cpp — The Hardware & Timing Engine (Core 1)
 * ============================================================================
 */

#include "task_sensor.h"
#include "state.h"
#include "../sensors/sensors.h"
#include "../utils/led_status.h"
#include <Arduino.h>

void sensor_task_loop(void *pvParameters) {
    // 1. Initialize physical sensors
    sensors_init();

    while(true) {
        ConfigState localCfg;
        SensorData  localData;

        // 2. Safely copy the configuration state (so we know what is turned ON/OFF)
        if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
            localCfg = currentConfig;
            xSemaphoreGive(stateMutex);
        }

        // Initialize local errors to false
        memset(localData.errors, 0, sizeof(localData.errors));

        // 3. Gather Data (Mock Data vs Real Hardware)
        if (localCfg.demo_mode) {
            // Generate realistic mock data for UI and Firebase testing
            localData.wl_percent = random(40, 80) + (random(0, 100) / 100.0);
            localData.light_lux  = random(500, 2000) + (random(0, 100) / 100.0);
            localData.tds_ppm    = random(150, 300) + (random(0, 100) / 100.0);
            localData.dht_temp   = random(22, 28) + (random(0, 100) / 100.0);
            localData.dht_hum    = random(50, 70) + (random(0, 100) / 100.0);
            localData.ph_val     = random(6, 8) + (random(0, 100) / 100.0);
            localData.w_temp     = random(20, 26) + (random(0, 100) / 100.0);
        } else {
            // Read physical hardware (defined in src/sensors/sensors.cpp)
            sensors_read_all(localData, localCfg);
        }

        // 4. Calculate VPD (Vapor Pressure Deficit) if DHT22 is active & healthy
        if (localCfg.sensor_enabled[S_DHT] && !localData.errors[S_DHT]) {
            float svp = 0.61078 * exp((17.27 * localData.dht_temp) / (localData.dht_temp + 237.3));
            float avp = svp * (localData.dht_hum / 100.0);
            localData.vpd_kpa = svp - avp;
        } else {
            localData.vpd_kpa = 0.0;
        }

        // 5. Advanced System-Level NeoPixel Logic
        bool hasSystemErrors = false;
        for (int i = 0; i < S_COUNT; i++) {
            if (localCfg.sensor_enabled[i] && localData.errors[i]) {
                hasSystemErrors = true;
                break;
            }
        }

        if (hasSystemErrors) {
            // If any sensor is failing, cycle through their specific error colors (Red, Purple, etc.)
            ledCycleErrors(localData.errors, localCfg.sensor_enabled);
        } else {
            // SYSTEM HEALTHY: Show Temperature Status (Cold/Normal/Hot)
            if (localCfg.sensor_enabled[S_DHT] || localCfg.sensor_enabled[S_WTEMP]) {
                float checkTemp = localCfg.sensor_enabled[S_WTEMP] ? localData.w_temp : localData.dht_temp;

                if (checkTemp < 20.0) {
                    ledSetSolid(0, 0, 255); // Blue = Cold
                } else if (checkTemp > 35.0) {
                    ledSetSolid(255, 0, 0); // Red = Hot
                } else {
                    ledSetSolid(0, 255, 0); // Green = Normal / Ideal
                }
            } else {
                ledSetSolid(0, 255, 0); // Fallback Green
            }
        }

        // 6. Safely push the fresh data to Global State for Core 0 to read
        if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
            currentData = localData;
            xSemaphoreGive(stateMutex);
        }

        // 7. Wait exactly 2 seconds before reading again
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
