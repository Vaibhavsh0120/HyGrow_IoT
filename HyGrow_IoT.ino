#include <Arduino.h>
#include <esp_system.h>
#include "src/core/state.h"
#include "src/core/task_network.h"
#include "src/core/task_sensor.h"
#include "src/utils/led_status.h"

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

// GPIO19 (USB D-) and GPIO20 (USB D+) are reserved by the native USB CDC
// stack on this board (see platformio.ini build_flags and config.h). If any
// configurable pin was ever saved as 19 or 20 — via a bad manual edit, a
// migration, or a factory-reset race — using it will fight the USB stack
// and look like the board randomly disconnecting from the serial monitor.
// Catch and neutralize that here, once, before any sensor touches a pin.
static void enforceForbiddenPins()
{
    struct PinRef
    {
        int *pin;
        const char *name;
    };
    PinRef pins[] = {
        {&currentConfig.pin_dht, "DHT22"},
        {&currentConfig.pin_ds18b20, "DS18B20"},
        {&currentConfig.pin_tds, "TDS"},
        {&currentConfig.pin_ph, "pH"},
        {&currentConfig.pin_lux_sda, "BH1750 SDA"},
        {&currentConfig.pin_lux_scl, "BH1750 SCL"},
        {&currentConfig.pin_wl, "Water Level Signal"},
        {&currentConfig.pin_wl_power, "Water Level Power"},
    };

    bool changed = false;
    for (auto &p : pins)
    {
        if (*p.pin == 19 || *p.pin == 20)
        {
            Serial.println("FORBIDDEN PIN: " + String(p.name) + " was configured on GPIO" +
                            String(*p.pin) + " (native USB D-/D+). Disabling this sensor to protect the USB stack.");
            *p.pin = -1;
            changed = true;
        }
    }

    if (changed)
    {
        state_save();
    }
}

// Task handles
TaskHandle_t NetworkTaskHandle;
TaskHandle_t SensorTaskHandle;

// Set true once initNetworkTask() has finished bringing up Wi-Fi/AP and the
// web server. The sensor task waits on this before it starts touching any
// hardware, so a slow/failing Wi-Fi connect (up to the 15s STA timeout,
// longer if it falls back to SoftAP) no longer races sensor init against
// network bring-up.
static volatile bool networkReady = false;

// Task wrappers for FreeRTOS
void networkTaskWrapper(void *parameter)
{
    initNetworkTask();
    networkReady = true;
    for (;;)
    {
        networkTaskLoop();
        vTaskDelay(10 / portTICK_PERIOD_MS); // Yield to IDLE task
    }
}

void sensorTaskWrapper(void *parameter)
{
    while (!networkReady)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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

    // 1a. LittleFS is mounted inside state_init(). A failed mount here means
    // the web UI (and everything served from it) is unavailable — continuing
    // to boot would silently start serving nothing, or a broken partial site.
    // Halt loudly instead so a bad/missing filesystem image is obvious.
    if (!currentVitals.littlefs_ok)
    {
        Serial.println("FATAL: LittleFS mount failed. The web UI cannot be served.");
        Serial.println("Re-flash the filesystem image (Upload Filesystem Image) and reset the board.");
        while (true)
        {
            delay(1000);
        }
    }

    // 1b. Neutralize any pin that landed on the reserved USB D-/D+ lines
    // before any sensor init code gets a chance to touch it.
    enforceForbiddenPins();

    // 1c. Bring up the status LED before either task starts, so it's
    // available the moment the sensor task begins reporting health.
    ledStatusInit();

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
