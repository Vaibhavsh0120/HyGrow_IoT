#include "led_status.h"
#include "../../config.h"
#include <Adafruit_NeoPixel.h>

Adafruit_NeoPixel pixel(1, PIN_RGB_LED, NEO_GRB + NEO_KHZ800);

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
    pixel.begin();
    pixel.setBrightness(30); // Keep low to avoid blinding
    pixel.clear();
    pixel.show();
}

void ledSetSolid(uint8_t r, uint8_t g, uint8_t b) {
    pixel.setPixelColor(0, pixel.Color(r, g, b));
    pixel.show();
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
                pixel.setPixelColor(0, ERROR_COLORS[cycleIndex]);
                pixel.show();
                return;
            }
            checked++;
        }
    }
}
