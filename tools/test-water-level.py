"""Run the real water-level reader against a simulated power gate and ADC.

Requires a host C++ compiler (g++). Only Arduino hardware calls are replaced;
the production sensor source is copied unchanged into a temporary workspace.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile


ARDUINO = r"""
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
using std::isnan;
constexpr int OUTPUT = 1, INPUT = 0, LOW = 0, HIGH = 1;
struct String : std::string {
    using std::string::string;
    String(int n) : std::string(std::to_string(n)) {}
};
void pinMode(int, int);
void digitalWrite(int, int);
void delay(unsigned long);
uint32_t analogReadMilliVolts(uint8_t);
"""

STATE = r"""
#pragma once
#include <Arduino.h>
struct Config { int pin_wl = 1; int pin_wl_power = 5; };
extern Config currentConfig;
constexpr int LOG_INFO = 0, LOG_ERR = 1;
void webLog(int, int, const std::string&);
"""

TEST = r"""
#include "src/core/state.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

Config currentConfig;
void sensor_wl_init();
bool sensor_wl_read(float&);
static bool powered = false;
static unsigned long now = 0, powerOn = 0, onDuration = 0, warmup = 0;
static unsigned sampleIndex = 0, pulses = 0;
static std::vector<uint32_t> samples;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
void pinMode(int, int) {}
void webLog(int, int, const std::string&) {}
void digitalWrite(int pin, int value) {
    require(pin == currentConfig.pin_wl_power, "Wrong power gate pin");
    if (value == HIGH) {
        require(!powered, "Probe was left powered between reads");
        powered = true; powerOn = now; ++pulses;
    } else {
        if (powered) onDuration = now - powerOn;
        powered = false;
    }
}
void delay(unsigned long ms) { now += ms; }
uint32_t analogReadMilliVolts(uint8_t pin) {
    require(pin == currentConfig.pin_wl, "Wrong signal pin");
    require(powered, "ADC sampled while probe was off");
    if (now - powerOn < warmup) return 0;
    return samples[std::min<size_t>(sampleIndex++, samples.size() - 1)];
}
static float read(std::vector<uint32_t> values, unsigned long settle = 0) {
    samples = values; sampleIndex = 0; warmup = settle;
    unsigned before = pulses;
    float percent = NAN;
    require(sensor_wl_read(percent), "Valid reading reported as failed");
    require(!powered, "Power must be off after every reading");
    require(pulses == before + 1, "Each reading must use one bounded power pulse");
    require(onDuration <= 100, "Probe energized too long for a sampling burst");
    require(std::isfinite(percent), "Reading must be finite");
    return percent;
}
int main() {
    float percent = 123;
    require(!sensor_wl_read(percent) && std::isnan(percent),
            "Uninitialized probe must not publish a false zero");
    sensor_wl_init();
    require(!powered, "Initialization must leave the probe unpowered");

    // A probe that needs 30 ms to settle must not publish its startup zero.
    require(std::fabs(read({1650}, 30) - 50.0f) < 0.01f,
            "Startup transient was plotted as an empty tank");

    // First conversion is stale; later samples contain low and high glitches.
    require(std::fabs(read({0, 1650, 1655, 0, 1645, 1660, 1650, 1650, 3300, 1650}) - 50.0f) < 0.01f,
            "Isolated ADC glitches reached telemetry");

    // Filtering within a burst must preserve real changes between cycles.
    require(std::fabs(read({330}) - 10.0f) < 0.01f,
            "A real falling level was hidden by the previous reading");
    require(read({0}) == 0.0f, "A genuinely empty tank must still report zero");
    require(read({3300}) == 100.0f, "Full tank must report 100 percent");
    require(read({4000}) == 100.0f, "Percentage must stay within bounds");
    std::cout << "Water-level settling, glitches, real changes, and power gating passed.\n";
}
"""


def main():
    compiler = shutil.which("g++")
    if not compiler:
        raise SystemExit("Install a host g++ compiler to run water-level checks.")
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="hygrow-water-level-") as folder:
        work = Path(folder)
        (work / "src" / "sensors").mkdir(parents=True)
        (work / "src" / "core").mkdir()
        shutil.copyfile(root / "src/sensors/sensor_water_level.cpp",
                        work / "src/sensors/sensor_water_level.cpp")
        (work / "Arduino.h").write_text(ARDUINO)
        (work / "src/core/state.h").write_text(STATE)
        (work / "test.cpp").write_text(TEST)
        executable = work / "water-level-test.exe"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-I", str(work),
                        str(work / "src/sensors/sensor_water_level.cpp"),
                        str(work / "test.cpp"), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
