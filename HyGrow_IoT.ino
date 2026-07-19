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
            webLog(0, LOG_WARN, "FORBIDDEN PIN: " + String(p.name) + " was configured on GPIO" +
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

// ----------------------------------------------------------------------------
// BOOT button (GPIO0) hold-to-reset
// ----------------------------------------------------------------------------
// GPIO0 is the same pin as the board's BOOT button, wired active-LOW with an
// external pull-up (standard on every ESP32/ESP32-S3 devkit). Held for:
//   - 10s: Auth Reset  — wipes ONLY the admin password/session token
//          (auth_reset(), state.cpp). Wi-Fi, sensors, and calibration are
//          left untouched. Blinks the LED Red x3 to confirm.
//   - 20s: Factory Reset — wipes all of NVS and reboots
//          (state_factory_reset(), state.cpp). Blinks the LED Red x5 to
//          confirm before the reboot.
// A held button is checked every BOOT_POLL_MS; the two thresholds are each
// fired exactly once per hold (latched via the fired flags below) so a very
// long hold doesn't trigger the 10s action repeatedly on its way to 20s, or
// fire the 20s action a second time if the button is held even longer.
#define BOOT_BUTTON_PIN 0
#define BOOT_POLL_MS 50
#define BOOT_HOLD_AUTH_RESET_MS 10000
#define BOOT_HOLD_FACTORY_RESET_MS 20000

void bootButtonTaskWrapper(void *parameter)
{
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    unsigned long pressStart = 0;
    bool pressed = false;
    bool authResetFired = false;
    bool factoryResetFired = false;

    for (;;)
    {
        bool down = (digitalRead(BOOT_BUTTON_PIN) == LOW);

        if (down && !pressed)
        {
            // Button just went down — start timing this hold.
            pressed = true;
            pressStart = millis();
            authResetFired = false;
            factoryResetFired = false;
        }
        else if (down && pressed)
        {
            unsigned long heldMs = millis() - pressStart;

            if (!factoryResetFired && heldMs >= BOOT_HOLD_FACTORY_RESET_MS)
            {
                factoryResetFired = true;
                webLog(0, LOG_WARN, "BOOT button held 20s: FACTORY RESET triggered.");
                ledBlink(255, 0, 0, 5); // Red x5, blocking — nothing else needs the CPU right now
                state_factory_reset();  // wipes all NVS and reboots; never returns
            }
            else if (!authResetFired && heldMs >= BOOT_HOLD_AUTH_RESET_MS)
            {
                authResetFired = true;
                webLog(0, LOG_WARN, "BOOT button held 10s: AUTH RESET triggered (password only).");
                ledBlink(255, 0, 0, 3); // Red x3
                auth_reset();
                webLog(0, LOG_WARN, "Admin password cleared. Wi-Fi/sensors/calibration untouched. Next page load will show Set Password.");
            }
        }
        else if (!down && pressed)
        {
            // Released — log how long it was actually held. This is the key
            // diagnostic for the "3 blinks at 10s, then 3 blinks again at 20s
            // instead of 5" symptom: that happens when contact bounces mid-hold
            // (a brief down->up->down blip the 50ms poll sees as a real
            // release), which silently resets pressStart on the next tick and
            // restarts the 10s count from zero. Without this log, that reset is
            // invisible — the user only sees the blink pattern and has no way
            // to tell "restarted the count" apart from "genuinely held to 20s".
            unsigned long heldMs = millis() - pressStart;
            pressed = false;

            if (factoryResetFired)
            {
                // Already handled: state_factory_reset() reboots and never
                // returns, so this branch is effectively unreachable, but
                // kept for clarity/symmetry with the other two cases.
            }
            else if (authResetFired)
            {
                webLog(0, LOG_INFO, "BOOT button released after " + String(heldMs) + "ms (auth reset already fired; kept holding but never reached 20s, or contact broke before then).");
            }
            else if (heldMs >= BOOT_POLL_MS)
            {
                // Only log ordinary short presses/taps if they're at least one
                // poll interval, to avoid spamming the log for pure electrical
                // noise shorter than the loop can even observe meaningfully.
                webLog(0, LOG_INFO, "BOOT button released after " + String(heldMs) + "ms (no reset triggered; hold 10s for auth reset, 20s for factory reset).");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
    }
}

// Task handles
TaskHandle_t NetworkTaskHandle;
TaskHandle_t SensorTaskHandle;
TaskHandle_t BootButtonTaskHandle;

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

    // PSRAM check — printed unconditionally, every boot. This is the ONLY
    // reliable way to confirm PSRAM is actually enabled: the "PLATFORM:"
    // and "HARDWARE:" lines PlatformIO prints during compilation come from
    // the esp32-s3-devkitc-1 board JSON's static metadata (that board
    // profile is the N8/no-PSRAM variant — platform-espressif32 has no
    // dedicated N16R8 profile) and never reflect the board_build.arduino.
    // memory_type / -DBOARD_HAS_PSRAM overrides in platformio.ini, so they
    // will keep printing "No PSRAM" at compile time even when PSRAM is
    // correctly enabled and working. psramFound() asks the chip directly,
    // at runtime, after boot — this is ground truth.
    if (psramFound())
    {
        Serial.printf("PSRAM: OK — %u bytes detected\n", ESP.getPsramSize());
    }
    else
    {
        Serial.println("PSRAM: NOT DETECTED — check board_build.arduino.memory_type in platformio.ini");
    }

    // Log WHY we rebooted. If this ever prints TASK_WDT or PANIC, the
    // previous boot crashed/hung — it did not just lose its USB connection.
    esp_reset_reason_t reason = esp_reset_reason();
    Serial.print("Reset reason: ");
    Serial.println(resetReasonToString(reason));

    // 1. Initialize NVS and load all variables into currentConfig
    state_init();

    // 1a''. Surface WHY the device restarted, in a way that survives past
    // this boot's Serial session. state_init() (just above) already loaded
    // the PREVIOUS boot's reason from its own NVS namespace before this
    // call overwrites it with the current one — so a browser that opens
    // the web Terminal well after a crash still sees, via the log ring
    // buffer, that (e.g.) "the board came back up because of a PANIC",
    // not just silence. webLog() itself already goes to Serial too, so
    // this doesn't duplicate the resetReasonToString() print below; it's
    // the boot BEFORE this one that's being reported here.
    {
        String lastReason = state_get_last_reset_reason();
        if (lastReason.length() > 0)
        {
            webLog(0, LOG_WARN, "Previous boot ended with: " + lastReason);
        }
        state_log_reset_reason(resetReasonToString(reason));
    }

    // 1a'. Mount the single-owner auth namespace and load the admin
    // password/session token into RAM. Deliberately separate from
    // state_init() above — see the long comment on the auth_*() decls in
    // state.h for why auth has its own NVS namespace and lifecycle. Must run
    // before the network task starts, since the WS gatekeeper
    // (handleAuthCommand() in auth.cpp) depends on it from the very
    // first client connection.
    auth_init();

    // 1a. LittleFS is mounted inside state_init(). A failed mount here means
    // the web UI (and everything served from it) is unavailable — continuing
    // to boot would silently start serving nothing, or a broken partial site.
    // Halt loudly instead so a bad/missing filesystem image is obvious.
    //
    // This halt happens BEFORE either FreeRTOS task is created (the
    // xTaskCreatePinnedToCore() calls are further down in setup()), and
    // state_factory_reset()/ESP.restart() are never reached from here either
    // — so with no filesystem, sensor init/reads and the network/web server
    // never start at all. Nothing "keeps checking sensors" once this branch
    // is taken; the board does nothing else but blink and wait to be
    // re-flashed or power-cycled.
    if (!currentVitals.littlefs_ok)
    {
        Serial.println("FATAL: LittleFS mount failed. The web UI cannot be served.");
        Serial.println("Sensor and network tasks will NOT be started — halting here.");
        Serial.println("Re-flash the filesystem image (Upload Filesystem Image) and reset the board.");
        ledStatusInit();
        // Distinct solid magenta — reserved exclusively for this failure
        // mode so it's never confused with a runtime sensor-error color or
        // the multi-sensor-failure white strobe. See ledFilesystemHaltSolid()
        // in led_status.cpp. Set once; the LED needs no further attention
        // since nothing else runs past this point.
        ledFilesystemHaltSolid();
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

    // 2. Pin Network Task to Core 0 (Handles Wi-Fi, the web server, WebSockets, LittleFS)
    // Stack size is 10240 to give HTTPClient/WiFiClientSecure (Firestore uploads)
    // and NVS operations comfortable headroom.
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

    // 4. BOOT button hold-to-reset watcher, pinned to Core 0 alongside the
    // network task. Tiny stack — it only does digitalRead()/millis() polling
    // and, on a long hold, calls into state.cpp/led_status.cpp.
    xTaskCreatePinnedToCore(
        bootButtonTaskWrapper,
        "BootButtonTask",
        2048,
        NULL,
        1,
        &BootButtonTaskHandle,
        0);
}

void loop()
{
    // FreeRTOS handles the tasks. We can just suspend the main loop task to save resources.
    vTaskDelete(NULL);
}
