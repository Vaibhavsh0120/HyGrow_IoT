/*
 * ============================================================================
 * sensor_water_level.cpp — Capacitive/Resistive Water Level Sensor
 * ============================================================================
 * Two-pin design: an analog signal pin (pin_wl) and a digital power-gate
 * pin (pin_wl_power). Most cheap water level strips are resistive and will
 * slowly corrode/electroplate their traces if left under constant voltage
 * while submerged. Gating power so the probe is only energized for the
 * short burst it takes to settle and take a reading — instead of being powered
 * 24/7 — is the standard mitigation and is what pin_wl_power is for.
 * ============================================================================
 */
#include "../core/state.h"
#include <Arduino.h>

// 12-bit ADC on ESP32-S3 (replaced with hardware mv)
#define MAX_VOLTAGE_MV 3300.0f

// Give the switched probe and its output time to settle. The old single
// conversion at 10ms could publish a power-on transient as an empty tank.
// A short median-filtered burst rejects isolated ADC spikes without hiding
// a genuinely empty tank or delaying changes across sensor cycles.
#define WL_SETTLE_MS 50
#define WL_SAMPLE_COUNT 9
#define WL_SAMPLE_GAP_MS 2

static bool s_wlReady = false;

void initWaterLevel()
{
    pinMode(currentConfig.pin_wl_power, OUTPUT);
    // Keep the probe unpowered until a read is actually requested — this is
    // the whole point of the power gate (minimize time under voltage).
    digitalWrite(currentConfig.pin_wl_power, LOW);

    pinMode(currentConfig.pin_wl, INPUT);

    s_wlReady = true;
    webLog(1, LOG_INFO, "Water level sensor initialized (Sig: " + String(currentConfig.pin_wl) +
                             ", Pwr: " + String(currentConfig.pin_wl_power) + ")");
}

void sensor_wl_init()
{
    initWaterLevel();
}

float readWaterLevel()
{
    // 1. Guard check: return NaN immediately if uninitialized, so
    // sensor_wl_read() correctly reports failure instead of a false "ok" at
    // 0.0. sensor_enabled[S_WL] is what actually decides whether this ever
    // gets called in practice — see validateSensor()/readAll() in
    // task_sensor.cpp.
    if (!s_wlReady)
    {
        return NAN;
    }

    // 2. Keep every conversion inside one bounded power pulse. Discard the
    // first conversion after power-on/channel switching, then collect a
    // burst. Cut power before filtering; the nominal on-time is ~66ms plus
    // ADC conversion time, with the probe off between sensor cycles.
    digitalWrite(currentConfig.pin_wl_power, HIGH);
    delay(WL_SETTLE_MS);

    (void)analogReadMilliVolts(currentConfig.pin_wl);
    int samples[WL_SAMPLE_COUNT];
    for (int i = 0; i < WL_SAMPLE_COUNT; i++)
    {
        samples[i] = analogReadMilliVolts(currentConfig.pin_wl);
        if (i + 1 < WL_SAMPLE_COUNT)
            delay(WL_SAMPLE_GAP_MS);
    }

    digitalWrite(currentConfig.pin_wl_power, LOW);

    // Sort this small stack buffer and use its middle value. Zero remains
    // a valid sample: sustained dry readings must still reach the graph.
    for (int i = 1; i < WL_SAMPLE_COUNT; i++)
    {
        int value = samples[i];
        int j = i - 1;
        while (j >= 0 && samples[j] > value)
        {
            samples[j + 1] = samples[j];
            j--;
        }
        samples[j + 1] = value;
    }
    int raw_mv = samples[WL_SAMPLE_COUNT / 2];

    // 3. Sanity check the raw ADC value. A dry/disconnected probe typically
    // floats near 0; a short or fully-submerged high-conductivity probe can
    // pin near the rail. Both extremes are still valid physical readings, so
    // we don't treat them as errors — just clamp the final percentage.
    if (raw_mv < 0)
    {
        webLog(1, LOG_ERR, "Water level ADC read failed!");
        return NAN;
    }

    // 4. Map raw ADC counts to a 0-100% float. Empty tank -> ~0 counts, full
    // tank -> ~MAX_VOLTAGE_MV counts, for a typical resistive strip probe wired as
    // a voltage divider against a fixed pull-down.
    float percent = (raw_mv / MAX_VOLTAGE_MV) * 100.0f;

    // 5. Clamp to a sane 0-100 range.
    if (percent < 0.0f)
        percent = 0.0f;
    else if (percent > 100.0f)
        percent = 100.0f;

    return percent;
}

bool sensor_wl_read(float &percent)
{
    float value = readWaterLevel();
    percent = value;
    return !isnan(value);
}
