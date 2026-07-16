/*
 * ============================================================================
 *  FINAL_PROJECT.ino — ESP32-S3 Sensor → Firestore Pipeline
 * ============================================================================
 *
 *  Main orchestrator for the modular sensor data pipeline.
 *
 *  Board:     ESP32-S3 N16R8
 *  Sensors:   Water Level, BH1750 Light, TDS, DHT22, pH, DS18B20
 *  Output:    Firebase Firestore (every 2 seconds)
 *  Feedback:  Built-in NeoPixel RGB LED (GPIO 48) — per-sensor error colors
 *
 *  Architecture:
 *    - config.h         → All settings in one place
 *    - led_status.*     → RGB LED error/status indicator
 *    - sensor_*.*       → One module per sensor (6 total)
 *    - firebase_handler.* → WiFi + Firestore communication
 *
 *  ──────────────────────────────────────────────────────────────────────
 *  SETUP CHECKLIST:
 *  1. Edit config.h with your WiFi and Firebase credentials
 *  2. Install required libraries (see implementation_plan or config.h)
 *  3. Select board: ESP32S3 Dev Module
 *  4. Wire sensors according to the pin assignments in config.h
 *  5. Upload and open Serial Monitor at 115200 baud
 *  ──────────────────────────────────────────────────────────────────────
 *
 *  Required Arduino Libraries:
 *    - FirebaseClient (Mobizt)
 *    - Adafruit NeoPixel
 *    - BH1750 (Christopher Laws)
 *    - DHT sensor library (Adafruit)
 *    - Adafruit Unified Sensor
 *    - OneWire (Paul Stoffregen)
 *    - DallasTemperature (Miles Burton)
 * ============================================================================
 */

#include <WiFi.h>
#include "config.h"
#include "src/utils/led_status.h"
#include "src/firebase/firebase_handler.h"
#include "src/sensors/sensor_water_level.h"
#include "src/sensors/sensor_light.h"
#include "src/sensors/sensor_tds.h"
#include "src/sensors/sensor_dht22.h"
#include "src/sensors/sensor_ph.h"
#include "src/sensors/sensor_water_temp.h"
#include "src/web/web_diagnose.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Global State
// ─────────────────────────────────────────────────────────────────────────────
static SensorData sensorData;
static unsigned long lastSendTime = 0;
static bool sensorInitStatus[SENSOR_COUNT] = { false };

// ─────────────────────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    // ── Serial ──
    Serial.begin(SERIAL_BAUD_RATE);
    delay(1000);  // Wait for serial monitor to connect

    DBGLN("");
    DBGLN("╔══════════════════════════════════════════════════════════════╗");
    DBGLN("║         ESP32-S3 Sensor → Firestore Pipeline                 ║");
    DBGLN("║         6 Sensors • 2s Interval • RGB LED Status             ║");
    DBGLN("╚══════════════════════════════════════════════════════════════╝");
    DBGLN("");

    // ── LED Status ──
    DBGLN("── Initializing RGB LED ──");
    ledStatusInit();
    ledStartupAnimation();
    DBGLN("");

    if (DEMO_MODE) {
        DBGLN("=========================================");
        DBGLN("    DEMO MODE ACTIVE");
        DBGLN("    Hardware sensors will be ignored.");
        DBGLN("=========================================");
        DBGLN("");
        for (int i = 0; i < SENSOR_COUNT; i++) {
            sensorInitStatus[i] = true; // Pretend all initialized
        }
    } else {
        // ── Initialize All Sensors ──
        DBGLN("── Initializing Sensors ──");

        // 1. Water Level
        sensorInitStatus[SENSOR_WATER_LEVEL] = waterLevel_init();
        if (!sensorInitStatus[SENSOR_WATER_LEVEL]) {
            DBGLN("  ✗ Water Level — FAILED");
            ledSetError(SENSOR_WATER_LEVEL);
            delay(500);
        } else {
            DBGLN("  ✓ Water Level — OK");
        }

        // 2. BH1750 Light
        sensorInitStatus[SENSOR_LIGHT] = light_init();
        if (!sensorInitStatus[SENSOR_LIGHT]) {
            DBGLN("  ✗ BH1750 Light — FAILED");
            ledSetError(SENSOR_LIGHT);
            delay(500);
        } else {
            DBGLN("  ✓ BH1750 Light — OK");
        }

        // 3. TDS
        sensorInitStatus[SENSOR_TDS] = tds_init();
        if (!sensorInitStatus[SENSOR_TDS]) {
            DBGLN("  ✗ TDS — FAILED");
            ledSetError(SENSOR_TDS);
            delay(500);
        } else {
            DBGLN("  ✓ TDS — OK");
        }

        // 4. DHT22 (takes ~2s for init due to warm-up)
        sensorInitStatus[SENSOR_DHT22] = dht22_init();
        if (!sensorInitStatus[SENSOR_DHT22]) {
            DBGLN("  ✗ DHT22 — FAILED");
            ledSetError(SENSOR_DHT22);
            delay(500);
        } else {
            DBGLN("  ✓ DHT22 — OK");
        }

        // 5. pH Sensor
        sensorInitStatus[SENSOR_PH] = ph_init();
        if (!sensorInitStatus[SENSOR_PH]) {
            DBGLN("  ✗ pH Sensor — FAILED");
            ledSetError(SENSOR_PH);
            delay(500);
        } else {
            DBGLN("  ✓ pH Sensor — OK");
        }

        // 6. DS18B20 Water Temp
        sensorInitStatus[SENSOR_WATER_TEMP] = waterTemp_init();
        if (!sensorInitStatus[SENSOR_WATER_TEMP]) {
            DBGLN("  ✗ DS18B20 Water Temp — FAILED");
            ledSetError(SENSOR_WATER_TEMP);
            delay(500);
        } else {
            DBGLN("  ✓ DS18B20 Water Temp — OK");
        }
    }

    DBGLN("");

    // ── Initialize Firebase ──
    DBGLN("── Initializing Firebase ──");
    if (!firebaseInit()) {
        DBGLN("  ✗ Firebase — FAILED");
        DBGLN("  Check WiFi credentials and Firebase config in config.h");
        ledSetFirebaseError();
        // Don't halt — keep trying in loop
    } else {
        DBGLN("  ✓ Firebase — Connected and authenticated");
    }

    DBGLN("");
    DBGLN("══════════════════════════════════════════════════════════════");
    DBGLN("  Setup complete! Starting sensor readings...");
    DBGF("  Interval: %d ms | Collection: %s\n", SENSOR_READ_INTERVAL_MS, FIRESTORE_COLLECTION);
    DBGLN("══════════════════════════════════════════════════════════════");
    DBGLN("");

#if ENABLE_WEB_DIAGNOSE
    webDiagnoseInit();
    DBGF("  Web Diagnose IP: http://%s\n", WiFi.localIP().toString().c_str());
    DBGLN("══════════════════════════════════════════════════════════════");
    DBGLN("");
#endif

    ledSetOK();
}

// ─────────────────────────────────────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    // Always process Firebase async tasks
    firebaseLoop();

#if ENABLE_WEB_DIAGNOSE
    webDiagnoseLoop(sensorData);
#endif

    // Check if it's time for a new reading cycle
    unsigned long interval = DEMO_MODE ? 2000 : 20000;
    unsigned long now = millis();
    if (now - lastSendTime < interval) {
        return;  // Not time yet
    }
    lastSendTime = now;

    // ── Read All Sensors ──
    memset(sensorData.sensorError, 0, sizeof(sensorData.sensorError));

    if (DEMO_MODE) {
        // Generate mock data for demo mode
        sensorData.waterLevelRaw = random(1000, 3000);
        sensorData.waterLevelPercent = random(40, 80) + (random(0, 100) / 100.0);
        sensorData.lightLux = random(500, 2000) + (random(0, 100) / 100.0);
        sensorData.waterTempC = random(20, 26) + (random(0, 100) / 100.0);
        sensorData.tdsPPM = random(150, 300) + (random(0, 100) / 100.0);
        sensorData.airTempC = random(22, 28) + (random(0, 100) / 100.0);
        sensorData.humidityPercent = random(50, 70) + (random(0, 100) / 100.0);
        sensorData.phValue = random(6, 8) + (random(0, 100) / 100.0);
    } else {
        // Read real sensors sequentially with delays to prevent ground looping interference

        // 1. Water Level
        if (sensorInitStatus[SENSOR_WATER_LEVEL]) {
            if (!waterLevel_read()) sensorData.sensorError[SENSOR_WATER_LEVEL] = true;
            sensorData.waterLevelRaw = waterLevel_getRaw();
            sensorData.waterLevelPercent = waterLevel_getPercent();
        } else {
            sensorData.sensorError[SENSOR_WATER_LEVEL] = true;
        }
        delay(500); // Isolate from next sensor

        // 2. BH1750 Light
        if (sensorInitStatus[SENSOR_LIGHT]) {
            if (!light_read()) sensorData.sensorError[SENSOR_LIGHT] = true;
            sensorData.lightLux = light_getLux();
        } else {
            sensorData.sensorError[SENSOR_LIGHT] = true;
        }
        delay(100);

        // 3. DS18B20 Water Temp (read BEFORE TDS and pH for temperature compensation)
        if (sensorInitStatus[SENSOR_WATER_TEMP]) {
            if (!waterTemp_read()) sensorData.sensorError[SENSOR_WATER_TEMP] = true;
            sensorData.waterTempC = waterTemp_getTemperature();
        } else {
            sensorData.sensorError[SENSOR_WATER_TEMP] = true;
            sensorData.waterTempC = 25.0;  // Default for compensation
        }
        delay(500); // Isolate from next sensor

        // 4. TDS (uses water temp for compensation)
        if (sensorInitStatus[SENSOR_TDS]) {
            if (!tds_read(sensorData.waterTempC)) sensorData.sensorError[SENSOR_TDS] = true;
            sensorData.tdsPPM = tds_getPPM();
        } else {
            sensorData.sensorError[SENSOR_TDS] = true;
        }
        delay(500); // Isolate from next sensor

        // 5. DHT22 Air Temp & Humidity
        if (sensorInitStatus[SENSOR_DHT22]) {
            if (!dht22_read()) sensorData.sensorError[SENSOR_DHT22] = true;
            sensorData.airTempC = dht22_getTemperature();
            sensorData.humidityPercent = dht22_getHumidity();
        } else {
            sensorData.sensorError[SENSOR_DHT22] = true;
        }
        delay(100);

        // 6. pH Sensor (uses water temp for compensation)
        if (sensorInitStatus[SENSOR_PH]) {
            if (!ph_read(sensorData.waterTempC)) sensorData.sensorError[SENSOR_PH] = true;
            sensorData.phValue = ph_getValue();
        } else {
            sensorData.sensorError[SENSOR_PH] = true;
        }
    }

    // ── Calculate VPD (Vapor Pressure Deficit) ──
    if (!sensorData.sensorError[SENSOR_DHT22]) {
        // Calculate Saturation Vapor Pressure (SVP) in kPa
        float svp = 0.61078 * exp((17.27 * sensorData.airTempC) / (sensorData.airTempC + 237.3));
        // Calculate Actual Vapor Pressure (AVP) in kPa
        float avp = svp * (sensorData.humidityPercent / 100.0);
        // Calculate VPD
        sensorData.vpdKpa = svp - avp;
    } else {
        sensorData.vpdKpa = 0;
    }

    // ── Update LED Status ──
    bool hasErrors = false;
    for (int i = 0; i < SENSOR_COUNT; i++) {
        if (sensorData.sensorError[i]) {
            hasErrors = true;
            break;
        }
    }

    if (hasErrors) {
        ledCycleErrors(sensorData.sensorError);
    } else {
        ledSetOK();
    }

    // ── Send to Firestore ──
    if (isFirebaseReady()) {
        if (!firebaseSendData(sensorData)) {
            DBGLN("[MAIN] Failed to send data to Firestore");
            ledSetFirebaseError();
        }
    } else {
        DBGLN("[MAIN] Firebase not ready — data not sent");
        ledSetFirebaseError();
    }

    // ── Debug Summary ──
    DBGLN("┌────────────────────────────────────────────────────────────┐");
    DBGF("│ Mode:         %s\n", DEMO_MODE ? "DEMO (Mock Data)" : "REAL (Sensor Data)");
    DBGF("│ Water Level:  %4d raw | %5.1f%%   %s\n",
         sensorData.waterLevelRaw, sensorData.waterLevelPercent,
         sensorData.sensorError[SENSOR_WATER_LEVEL] ? "⚠ ERR" : "✓");
    DBGF("│ Light:        %8.1f lux           %s\n",
         sensorData.lightLux,
         sensorData.sensorError[SENSOR_LIGHT] ? "⚠ ERR" : "✓");
    DBGF("│ TDS:          %8.1f ppm           %s\n",
         sensorData.tdsPPM,
         sensorData.sensorError[SENSOR_TDS] ? "⚠ ERR" : "✓");
    DBGF("│ Air Temp:     %8.1f °C            %s\n",
         sensorData.airTempC,
         sensorData.sensorError[SENSOR_DHT22] ? "⚠ ERR" : "✓");
    DBGF("│ Humidity:     %8.1f %%             %s\n",
         sensorData.humidityPercent,
         sensorData.sensorError[SENSOR_DHT22] ? "⚠ ERR" : "✓");
    DBGF("│ VPD:          %8.2f kPa           %s\n",
         sensorData.vpdKpa,
         sensorData.sensorError[SENSOR_DHT22] ? "⚠ ERR" : "✓");
    DBGF("│ pH:           %8.2f               %s\n",
         sensorData.phValue,
         sensorData.sensorError[SENSOR_PH] ? "⚠ ERR" : "✓");
    DBGF("│ Water Temp:   %8.2f °C            %s\n",
         sensorData.waterTempC,
         sensorData.sensorError[SENSOR_WATER_TEMP] ? "⚠ ERR" : "✓");
    DBGF("│ WiFi RSSI:    %4d dBm\n", getWiFiRSSI());
    DBGLN("└────────────────────────────────────────────────────────────┘");
    DBGLN("");
}
