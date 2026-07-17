#include <Arduino.h>
#include "src/core/state.h"
#include "src/core/task_network.h"
#include "src/core/task_sensor.h"

// Task handles
TaskHandle_t NetworkTaskHandle;
TaskHandle_t SensorTaskHandle;

// Task wrappers for FreeRTOS
void networkTaskWrapper(void *parameter)
{
    initNetworkTask();
    for (;;)
    {
        networkTaskLoop();
        vTaskDelay(10 / portTICK_PERIOD_MS); // Yield to IDLE task
    }
}

void sensorTaskWrapper(void *parameter)
{
    initSensorTask();
    for (;;)
    {
        sensorTaskLoop();
        vTaskDelay(10 / portTICK_PERIOD_MS); // Yield to IDLE task
    }
}

void setup()
{
    Serial.begin(115200);
    delay(1000); // Give serial monitor time to connect
    Serial.println("\n--- HyGrow IoT Booting ---");

    // 1. Initialize NVS and load all variables into currentConfig
    state_init();

    // 2. Pin Network Task to Core 0 (Handles Wi-Fi, OTA, WebSockets, LittleFS)
    // Increased stack size to 10240 to handle ElegantOTA and NVS operations safely
    xTaskCreatePinnedToCore(
        networkTaskWrapper,
        "NetworkTask",
        10240,
        NULL,
        1,
        &NetworkTaskHandle,
        0);

    // 3. Pin Sensor Task to Core 1 (Handles analog reads, DHT, I2C)
    xTaskCreatePinnedToCore(
        sensorTaskWrapper,
        "SensorTask",
        4096,
        NULL,
        1,
        &SensorTaskHandle,
        1);
}

void loop()
{
    // FreeRTOS handles the tasks. We can just suspend the main loop task to save resources.
    vTaskDelete(NULL);
}
