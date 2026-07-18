#pragma once
#include <Arduino.h>

void ledStatusInit();
void ledSetSolid(uint8_t r, uint8_t g, uint8_t b);
// Turns the status LED off. Used when every enabled sensor's last read
// succeeded — the LED only ever lights up to signal a problem now, so no
// light means "healthy," not "unknown"/"still starting."
void ledStatusOff();
void ledCycleErrors(const bool sensorErrors[], const bool sensorEnabled[]);

// Blocking blink of `times` on/off cycles at the given color, `onMs`/`offMs`
// each. Used for BOOT-button hold feedback (auth reset / factory reset),
// where the whole board is otherwise idle waiting for the button release —
// blocking here is acceptable because nothing else needs the CPU at that
// moment. Leaves the LED off when done.
void ledBlink(uint8_t r, uint8_t g, uint8_t b, uint8_t times, uint16_t onMs = 150, uint16_t offMs = 150);
