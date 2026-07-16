/*
 * ============================================================================
 * state.cpp — Implementation of NVS persistence and Mutexes
 * ============================================================================
 */

#include "state.h"
#include "../../secrets.h" // Fallback default credentials
#include <stdarg.h>

ConfigState currentConfig;
SensorData  currentData;
SemaphoreHandle_t stateMutex;
Preferences prefs;

// This function will be defined in task_network.cpp (Core 0)
extern void broadcastLog(String msg);

void state_init() {
    // 1. Create the FreeRTOS Mutex
    stateMutex = xSemaphoreCreateMutex();
    if (stateMutex == NULL) {
        Serial.println("CRITICAL ERROR: Failed to create state Mutex!");
        while(1); // Halt
    }

    // 2. Open NVS namespace "hygrow" in Read/Write mode
    prefs.begin("hygrow", false);

    // 3. Load variables (with fallbacks to secrets.h or defaults)
    String ssid = prefs.getString("ssid", SECRET_WIFI_SSID);
    String pass = prefs.getString("pass", SECRET_WIFI_PASSWORD);

    strncpy(currentConfig.wifi_ssid, ssid.c_str(), sizeof(currentConfig.wifi_ssid));
    strncpy(currentConfig.wifi_password, pass.c_str(), sizeof(currentConfig.wifi_password));

    currentConfig.demo_mode = prefs.getBool("demo", true);

    // Load individual sensor toggles (Default to true)
    for(int i = 0; i < S_COUNT; i++) {
        currentConfig.sensor_enabled[i] = prefs.getBool(("s_en_" + String(i)).c_str(), true);
    }

    // Initialize sensor data to zero
    memset(&currentData, 0, sizeof(SensorData));
}

void state_save() {
    // Write current configuration back to NVS flash
    prefs.putString("ssid", currentConfig.wifi_ssid);
    prefs.putString("pass", currentConfig.wifi_password);
    prefs.putBool("demo", currentConfig.demo_mode);

    for(int i = 0; i < S_COUNT; i++) {
        prefs.putBool(("s_en_" + String(i)).c_str(), currentConfig.sensor_enabled[i]);
    }

    webLog("[SYS] Configuration saved to NVS.");
}

void webLog(const char* format, ...) {
    // Format the string like standard printf
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Also print to standard Serial for traditional debugging
    Serial.println(buffer);

    // Send to WebSockets (UI Terminal)
    broadcastLog(String(buffer));
}
