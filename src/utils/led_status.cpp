/*
 * ============================================================================
 *  led_status.cpp — RGB NeoPixel LED Status Indicator Implementation
 * ============================================================================
 */

#include "led_status.h"
#include <Adafruit_NeoPixel.h>

// ─────────────────────────────────────────────────────────────────────────────
//  NeoPixel Instance
// ─────────────────────────────────────────────────────────────────────────────
static Adafruit_NeoPixel pixel(RGB_LED_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

// ─────────────────────────────────────────────────────────────────────────────
//  Error Color Table — indexed by SensorID
// ─────────────────────────────────────────────────────────────────────────────
struct RGBColor {
    uint8_t r, g, b;
};

static const RGBColor sensorErrorColors[SENSOR_COUNT] = {
    { 255,   0,   0 },   // SENSOR_WATER_LEVEL → Red
    { 255, 255,   0 },   // SENSOR_LIGHT       → Yellow
    { 128,   0, 255 },   // SENSOR_TDS         → Purple
    { 255,  80,   0 },   // SENSOR_DHT22       → Orange
    {   0,   0, 255 },   // SENSOR_PH          → Blue
    {   0, 255, 255 },   // SENSOR_WATER_TEMP  → Cyan
};

static const RGBColor colorOK       = {   0, 255,   0 };   // Green
static const RGBColor colorFirebase = { 255, 255, 255 };   // White

// Track cycling state
static uint8_t cycleIndex = 0;
static unsigned long lastCycleTime = 0;
static const unsigned long CYCLE_INTERVAL_MS = 300;

// ─────────────────────────────────────────────────────────────────────────────
//  Private Helpers
// ─────────────────────────────────────────────────────────────────────────────
static void setColor(uint8_t r, uint8_t g, uint8_t b) {
    pixel.setPixelColor(0, pixel.Color(r, g, b));
    pixel.show();
}

static void setColor(const RGBColor &c) {
    setColor(c.r, c.g, c.b);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public Implementation
// ─────────────────────────────────────────────────────────────────────────────

void ledStatusInit() {
    pixel.begin();
    pixel.setBrightness(RGB_LED_BRIGHTNESS);
    pixel.clear();
    pixel.show();
    DBGLN("[LED] NeoPixel initialized on GPIO " + String(RGB_LED_PIN));
}

void ledSetError(SensorID id) {
    if (id < SENSOR_COUNT) {
        setColor(sensorErrorColors[id]);
    } else if (id == SENSOR_FIREBASE) {
        ledSetFirebaseError();
    }
}

void ledSetOK() {
    setColor(colorOK);
}

void ledSetFirebaseError() {
    // White blink effect
    static bool blinkState = false;
    blinkState = !blinkState;
    if (blinkState) {
        setColor(colorFirebase);
    } else {
        setColor(0, 0, 0);
    }
}

void ledClear() {
    pixel.clear();
    pixel.show();
}

void ledCycleErrors(bool errors[]) {
    // Count active errors
    uint8_t errorCount = 0;
    uint8_t errorIndices[SENSOR_COUNT];
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        if (errors[i]) {
            errorIndices[errorCount++] = i;
        }
    }

    if (errorCount == 0) {
        ledSetOK();
        return;
    }

    if (errorCount == 1) {
        // Single error — solid color
        setColor(sensorErrorColors[errorIndices[0]]);
        return;
    }

    // Multiple errors — cycle through them
    unsigned long now = millis();
    if (now - lastCycleTime >= CYCLE_INTERVAL_MS) {
        lastCycleTime = now;
        cycleIndex = (cycleIndex + 1) % errorCount;
    }
    setColor(sensorErrorColors[errorIndices[cycleIndex]]);
}

void ledStartupAnimation() {
    DBGLN("[LED] Startup animation...");
    
    // Sweep through all sensor error colors
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        setColor(sensorErrorColors[i]);
        delay(150);
    }
    
    // Flash green 3 times
    for (int i = 0; i < 3; i++) {
        setColor(colorOK);
        delay(100);
        ledClear();
        delay(100);
    }
    
    // Settle on green
    setColor(colorOK);
}
