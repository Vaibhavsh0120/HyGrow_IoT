#include "led_status.h"
#include "../../config.h"
#include <Adafruit_NeoPixel.h>

Adafruit_NeoPixel pixel(1, PIN_RGB_LED, NEO_GRB + NEO_KHZ800);

// Adafruit_NeoPixel is not reentrant/thread-safe, but this single pixel is
// now driven from two different FreeRTOS tasks on two different cores: the
// sensor task (task_sensor.cpp, core 1, via ledCycleErrors()/ledSetSolid())
// and the BOOT-button watcher (HyGrow_IoT.ino, core 0, via ledBlink() during
// a 10s/20s hold). Without a lock, a hold-to-reset blink could interleave
// its pixel.show() calls with the sensor task's, corrupting a frame or (in
// the worst case) racing the underlying RMT/bit-bang driver state. Every
// public function here takes this mutex for the duration of its pixel
// writes to serialize the two tasks instead.
static SemaphoreHandle_t ledMutex = nullptr;

static void ledLock() {
    if (ledMutex) xSemaphoreTake(ledMutex, portMAX_DELAY);
}
static void ledUnlock() {
    if (ledMutex) xSemaphoreGive(ledMutex);
}

// Color Map matching your specification:
// Red(WL), Yellow(LIGHT), Purple(TDS), Orange(DHT), Blue(PH), Cyan(WTEMP)
const uint32_t ERROR_COLORS[] = {
    pixel.Color(255, 0, 0),     // S_WL
    pixel.Color(255, 255, 0),   // S_LIGHT
    pixel.Color(128, 0, 255),   // S_TDS
    pixel.Color(255, 80, 0),    // S_DHT
    pixel.Color(0, 0, 255),     // S_PH
    pixel.Color(0, 255, 255)    // S_WTEMP
};

void ledStatusInit() {
    if (!ledMutex) ledMutex = xSemaphoreCreateMutex();
    ledLock();
    pixel.begin();
    pixel.setBrightness(30); // Keep low to avoid blinding
    pixel.clear();
    pixel.show();
    ledUnlock();
}

void ledSetSolid(uint8_t r, uint8_t g, uint8_t b) {
    ledLock();
    pixel.setPixelColor(0, pixel.Color(r, g, b));
    pixel.show();
    ledUnlock();
}

void ledStatusOff() {
    ledLock();
    pixel.clear();
    pixel.show();
    ledUnlock();
}

void ledBlink(uint8_t r, uint8_t g, uint8_t b, uint8_t times, uint16_t onMs, uint16_t offMs) {
    for (uint8_t i = 0; i < times; i++) {
        ledLock();
        pixel.setPixelColor(0, pixel.Color(r, g, b));
        pixel.show();
        ledUnlock();
        delay(onMs);
        ledLock();
        pixel.clear();
        pixel.show();
        ledUnlock();
        delay(offMs);
    }
}

void ledCycleErrors(const bool sensorErrors[], const bool sensorEnabled[]) {
    static uint8_t cycleIndex = 0;
    static unsigned long lastCycleTime = 0;

    // Cycle to the next broken sensor color every 500ms
    if (millis() - lastCycleTime > 500) {
        lastCycleTime = millis();

        uint8_t checked = 0;
        while(checked < S_COUNT) {
            cycleIndex = (cycleIndex + 1) % S_COUNT;
            if (sensorEnabled[cycleIndex] && sensorErrors[cycleIndex]) {
                ledLock();
                pixel.setPixelColor(0, ERROR_COLORS[cycleIndex]);
                pixel.show();
                ledUnlock();
                return;
            }
            checked++;
        }
    }
}
