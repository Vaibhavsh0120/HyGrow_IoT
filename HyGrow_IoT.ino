/*
 * ============================================================================
 * HyGrow_IoT.ino — Dual-Core ESP32-S3 Sensor & Web Pipeline
 * ============================================================================
 * Core 0: WiFi, LittleFS WebServer, WebSockets, Firebase Uploads.
 * Core 1: Sensor Readings, Timing Loops, NeoPixel Status.
 * ============================================================================
 */

#include <Arduino.h>
#include "src/core/state.h"
#include "src/core/task_network.h"
#include "src/core/task_sensor.h"

// FreeRTOS Task Handles
TaskHandle_t NetworkTaskHandle;
TaskHandle_t SensorTaskHandle;

void setup() {
    // 1. Initialize Serial purely for boot debugging and Web UI IP address
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n╔══════════════════════════════════════════════════════════════╗");
    Serial.println("║                HyGrow-IoT Booting (Dual-Core)                ║");
    Serial.println("╚══════════════════════════════════════════════════════════════╝\n");

    // 2. Initialize Global State (Mutexes & NVS Preferences)
    state_init();

    // 3. Create Network & Web UI Task on Core 0
    // Stack size: 16384 bytes (Required for Firebase & WebServer)
    xTaskCreatePinnedToCore(
        network_task_loop,   // Task function
        "NetworkTask",       // Task name
        16384,               // Stack size
        NULL,                // Parameters
        1,                   // Priority
        &NetworkTaskHandle,  // Task handle
        0                    // Core ID (0 = Network/WiFi)
    );

    // 4. Create Hardware & Sensor Task on Core 1
    // Stack size: 8192 bytes
    xTaskCreatePinnedToCore(
        sensor_task_loop,    // Task function
        "SensorTask",        // Task name
        8192,                // Stack size
        NULL,                // Parameters
        1,                   // Priority
        &SensorTaskHandle,   // Task handle
        1                    // Core ID (1 = Application/Hardware)
    );

    // 5. Delete the setup task so FreeRTOS takes full control
    vTaskDelete(NULL);
}

void loop() {
    // Intentionally left empty. FreeRTOS handles the loops in the tasks.
}
