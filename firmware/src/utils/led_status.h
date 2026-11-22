#pragma once
#include <Arduino.h>

void ledStatusInit();
void ledSetSolid(uint8_t r, uint8_t g, uint8_t b);
// Turns the status LED off. Used when every enabled sensor's last read
// succeeded — the LED only ever lights up to signal a problem now, so no
// light means "healthy," not "unknown"/"still starting."
void ledStatusOff();
void ledCycleErrors(const bool sensorErrors[], const bool sensorEnabled[]);

// Multiple enabled sensors failed at once (2 or more). Distinct from the
// single-sensor per-color cycle in ledCycleErrors() on purpose — a fast
// solid-white strobe reads unambiguously as "several things are wrong,
// go check the Terminal" without the person needing to first watch a full
// ~3s color-cycle to count how many colors show up. Non-blocking: call this
// every sensor-task cycle exactly like ledCycleErrors(), it self-times off
// millis(). OFF/disabled sensors never factor in here — see the
// enabledErrorCount gate at the sensorTaskLoop() call site.
void ledMultiSensorFailure();

// Distinct halt indicator for "LittleFS mount failed at boot" — deliberately
// different from every runtime error signal above (which are all sensor
// colors or the white multi-failure strobe): a SOLID magenta, not blinking.
// Magenta is otherwise unused by any sensor color or the strobe, so it
// unambiguously means exactly one thing on this board. Call once; unlike
// the old blinking version this does not need to be called in a loop.
void ledFilesystemHaltSolid();

// Blocking blink of `times` on/off cycles at the given color, `onMs`/`offMs`
// each. Used for BOOT-button hold feedback (auth reset / factory reset),
// where the whole board is otherwise idle waiting for the button release —
// blocking here is acceptable because nothing else needs the CPU at that
// moment. Leaves the LED off when done.
void ledBlink(uint8_t r, uint8_t g, uint8_t b, uint8_t times, uint16_t onMs = 150, uint16_t offMs = 150);
