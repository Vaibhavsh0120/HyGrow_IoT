#include <Arduino.h>
#include <esp_system.h>
#include "src/core/state.h"
#include "src/core/task_network.h"
#include "src/core/task_sensor.h"

// Human-readable label for esp_reset_reason(), so a crash/watchdog reboot
// is obvious in the serial log instead of just showing up as a silent
// USB CDC disconnect/reconnect.
static const char *resetReasonToString(esp_reset_reason_t reason)
{
    switch (reason)
    {
    case ESP_RST_POWERON:
        return "POWERON (normal power-up)";
    case ESP_RST_EXT:
        return "EXT (external pin reset)";
    case ESP_RST_SW:
        return "SW (esp_restart() / software reset)";
    case ESP_RST_PANIC:
        return "PANIC (exception / crash)";
    case ESP_RST_INT_WDT:
        return "INT_WDT (interrupt watchdog timeout)";
    case ESP_RST_TASK_WDT:
        return "TASK_WDT (task watchdog timeout — likely a blocked/hung task)";
    case ESP_RST_WDT:
        return "WDT (other watchdog timeout)";
    case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP (woke from deep sleep)";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT (voltage dropped too low)";
    case ESP_RST_SDIO:
        return "SDIO reset";
    default:
        return "UNKNOWN";
    }
}

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

    // Log WHY we rebooted. If this ever prints TASK_WDT or PANIC, the
    // previous boot crashed/hung — it did not just lose its USB connection.
    esp_reset_reason_t reason = esp_reset_reason();
    Serial.print("Reset reason: ");
    Serial.println(resetReasonToString(reason));

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
