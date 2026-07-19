/*
 * ============================================================================
 * sensor_water_level.cpp — Capacitive/Resistive Water Level Sensor
 * ============================================================================
 * Two-pin design: an analog signal pin (pin_wl) and a digital power-gate
 * pin (pin_wl_power). Most cheap water level strips are resistive and will
 * slowly corrode/electroplate their traces if left under constant voltage
 * while submerged. Gating power so the probe is only energized for the
 * ~10ms it takes to settle and take a reading — instead of being powered
 * 24/7 — is the standard mitigation and is what pin_wl_power is for.
 * ============================================================================
 */
#include "../core/state.h"
#include <Arduino.h>

// 12-bit ADC on ESP32-S3 (replaced with hardware mv)
#define MAX_VOLTAGE_MV 3300.0f

// Time for the probe to settle after power is applied, before we trust the
// analog reading. 10ms is generous for the RC time constant of a resistive
// strip probe with a few hundred ohms/kohm impedance.
#define WL_SETTLE_MS 10

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

    // 2. Power the probe, wait for the reading to settle, sample, then cut
    // power again immediately. This is the anti-corrosion measure: the probe
    // is only ever live for ~WL_SETTLE_MS + one ADC read per sensor cycle,
    // not continuously.
    digitalWrite(currentConfig.pin_wl_power, HIGH);
    delay(WL_SETTLE_MS);

    int raw_mv = analogReadMilliVolts(currentConfig.pin_wl);

    digitalWrite(currentConfig.pin_wl_power, LOW);

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
