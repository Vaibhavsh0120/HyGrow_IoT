/*
 * ============================================================================
 * task_sensor.cpp — Sensor Task (Core 1)
 * ============================================================================
 * Single source of truth for all sensor hardware interaction. Owns:
 *   1. Boot-time init of every sensor, including sole ownership of the I2C
 *      bus (Wire.begin() + the BH1750 presence probe) — see
 *      bringUpI2CAndProbeLight() below. sensor_light.cpp no longer touches
 *      Wire.begin()/timeouts/probing itself; it assumes the bus is already
 *      live by the time sensor_lux_init() runs.
 *   2. Startup validation: each enabled sensor gets up to 5 read attempts
 *      before the task commits to it being "live". A sensor that fails all
 *      5 boot attempts is auto-disabled (sensor_enabled[] -> false) and
 *      persisted, so a genuinely unwired/dead sensor doesn't spam "read
 *      failed" into the web terminal forever. Its pin assignment is left
 *      untouched — auto-disable only ever flips the enabled flag.
 *   3. The steady-state read loop.
 *   4. The WS2812 status LED, updated every read cycle to reflect live
 *      sensor health: LED off when every enabled sensor is healthy, the
 *      per-sensor error color cycling when exactly one enabled sensor's
 *      last read failed, and a distinct fast white strobe
 *      (ledMultiSensorFailure(), led_status.cpp) when TWO OR MORE enabled
 *      sensors failed in the same cycle. Disabled/OFF sensors are never
 *      counted or shown here, regardless of their last_err[] state.
 * ============================================================================
 */
#include "task_sensor.h"
#include "state.h"
#include "../utils/led_status.h"
#include <Wire.h>
#include <math.h>

static volatile bool s_kick = false;
static bool s_initialized = false;

// BH1750 default I2C address (ADDR pin low/floating). Used only for the
// bounded presence probe below, before we hand off to the BH1750 library.
#define BH1750_PROBE_ADDR 0x23

// Number of boot-time read attempts before an enabled-but-unresponsive
// sensor is auto-disabled. Kept small so a genuinely missing sensor doesn't
// meaningfully delay the rest of boot, but large enough to ride out a
// sensor's normal warm-up/first-read jitter (e.g. DHT22's ~2s duty cycle).
#define STARTUP_RETRY_COUNT 5
#define STARTUP_RETRY_DELAY_MS 250

// VPD (kPa) from temp (°C) and RH (%)
static float computeVPD(float tC, float rh)
{
  if (isnan(tC) || isnan(rh))
    return 0.0f;
  float es = 0.6108f * expf((17.27f * tC) / (tC + 237.3f));
  float ea = es * (rh / 100.0f);
  float vpd = es - ea;
  if (vpd < 0)
    vpd = 0;
  return vpd;
}

// ----------------------------------------------------------------------------
// Demo mode simulation
// ----------------------------------------------------------------------------
// Demo Mode is a per-sensor HARDWARE fact, not just a global UI flag: a
// sensor is "in demo mode" if and only if its own pin field(s) currently
// equal DEMO_MODE_PIN (config.h) — see sensorPinIsDemo() just below. In
// practice every sensor's pins move together (save_features,
// command_handlers.cpp, swaps all of them at once when demo_mode is
// toggled), so this is equivalent to the old single global check in the
// common case — but it's now literally true at the pin level instead of only
// true by UI convention, which also means a sensor's pin could in principle
// be hand-set to the sentinel independently of the global flag (e.g. a
// hand-crafted save_pins message) and it would still correctly read as
// simulated here, rather than trying to bit-bang real hardware on a pin
// number that was never a real GPIO to begin with.
//
// readAllDemo() generates plausible fake readings — a slow random-walk
// around realistic hydroponic setpoints — so the dashboard looks "alive" for
// someone testing the UI with zero sensors wired up.
static bool sensorPinIsDemo(SensorID id)
{
  switch (id)
  {
    case S_DHT:
      return currentConfig.pin_dht == DEMO_MODE_PIN;
    case S_WTEMP:
      return currentConfig.pin_ds18b20 == DEMO_MODE_PIN;
    case S_TDS:
      return currentConfig.pin_tds == DEMO_MODE_PIN;
    case S_PH:
      return currentConfig.pin_ph == DEMO_MODE_PIN;
    case S_LIGHT:
      // BH1750 uses a two-wire I2C bus (SDA+SCL) — either pin landing on
      // the sentinel is enough to call this sensor "in demo mode"; save_features
      // always moves both together, so this only ever differs from "both"
      // on a hand-crafted/partial save_pins message, and treating that as
      // demo is the safer read (never tries to drive a bus with one real
      // and one fake pin).
      return currentConfig.pin_lux_sda == DEMO_MODE_PIN || currentConfig.pin_lux_scl == DEMO_MODE_PIN;
    case S_WL:
      // Same reasoning as S_LIGHT: water level has a signal pin plus a
      // separate power-gate pin.
      return currentConfig.pin_wl == DEMO_MODE_PIN || currentConfig.pin_wl_power == DEMO_MODE_PIN;
    default:
      return false;
  }
}
struct DemoWalkState
{
  float value;
  float target;
};
static DemoWalkState s_demoPh = {6.2f, 6.2f};
static DemoWalkState s_demoTds = {1000.0f, 1000.0f};
static DemoWalkState s_demoAirTemp = {24.0f, 24.0f};
static DemoWalkState s_demoHumidity = {62.0f, 62.0f};
static DemoWalkState s_demoWaterTemp = {22.0f, 22.0f};
static DemoWalkState s_demoLux = {450.0f, 450.0f};
static DemoWalkState s_demoWl = {65.0f, 65.0f};
static bool s_demoSeeded = false;

// Nudges `state.value` a small random step toward a freshly (rarely)
// re-rolled `state.target`, clamped to [lo, hi]. Produces a gentle
// slow-drifting "sine/random-walk" look rather than jumpy noise.
static float demoStep(DemoWalkState &state, float lo, float hi, float maxStep, float retargetChancePct)
{
  if ((rand() % 1000) < (int)(retargetChancePct * 10.0f))
  {
    float span = hi - lo;
    state.target = lo + span * 0.25f + (float)(rand() % 1000) / 1000.0f * (span * 0.5f);
  }

  float diff = state.target - state.value;
  float step = diff * 0.05f;
  float jitter = ((float)(rand() % 200) / 100.0f - 1.0f) * maxStep * 0.3f;
  state.value += step + jitter;

  if (state.value < lo)
    state.value = lo;
  if (state.value > hi)
    state.value = hi;

  return state.value;
}

// Forward declaration — readAllDemo() (below) now calls this directly
// (previously it set currentSensors.last_ok_ms/last_err inline for every
// sensor unconditionally; the per-sensor-gated version needs the same
// helper the real-mode readAll() already uses), but markOk() itself is
// defined further down this file.
static void markOk(SensorID id);

// Populates currentSensors with one simulated reading for every sensor that
// is BOTH enabled AND currently in demo mode (sensorPinIsDemo(), above), and
// marks each of those OK. A sensor whose sensor_enabled[] flag is off is
// skipped entirely, exactly like real-mode readAll() skips it — demo mode
// simulates hardware, it does not simulate having sensors that were
// explicitly turned off. A skipped sensor's last_ok_ms/last_err are left
// untouched so it shows as "no data", not fabricated data.
//
// In the common case every enabled sensor's pin moves to DEMO_MODE_PIN
// together (save_features flips them all at once), so this reads the same
// as "demo mode is on" — but checking per-sensor here means a sensor that
// was individually pinned back to a real GPIO (a hand-crafted save_pins
// message) correctly falls through to readAll()'s real-hardware branch for
// that one sensor instead, rather than being forced into simulation by a
// stale global flag.
static void readAllDemo()
{
  if (!s_demoSeeded)
  {
    randomSeed(esp_random());
    s_demoSeeded = true;
  }

  if (currentConfig.sensor_enabled[S_DHT] && sensorPinIsDemo(S_DHT))
  {
    float airTemp = demoStep(s_demoAirTemp, 22.0f, 26.0f, 0.3f, 1.5f);
    float humidity = demoStep(s_demoHumidity, 55.0f, 70.0f, 0.6f, 1.5f);
    currentSensors.temp_c = airTemp;
    currentSensors.humidity = humidity;
    currentSensors.vpd_kpa = computeVPD(airTemp, humidity);
    markOk(S_DHT);
  }

  if (currentConfig.sensor_enabled[S_WTEMP] && sensorPinIsDemo(S_WTEMP))
  {
    currentSensors.water_temp_c = demoStep(s_demoWaterTemp, 20.0f, 24.0f, 0.2f, 1.5f);
    markOk(S_WTEMP);
  }

  if (currentConfig.sensor_enabled[S_TDS] && sensorPinIsDemo(S_TDS))
  {
    currentSensors.tds_ppm = demoStep(s_demoTds, 800.0f, 1200.0f, 15.0f, 2.0f);
    markOk(S_TDS);
  }

  if (currentConfig.sensor_enabled[S_PH] && sensorPinIsDemo(S_PH))
  {
    currentSensors.ph_val = demoStep(s_demoPh, 6.0f, 6.5f, 0.05f, 2.0f);
    markOk(S_PH);
  }

  if (currentConfig.sensor_enabled[S_LIGHT] && sensorPinIsDemo(S_LIGHT))
  {
    currentSensors.lux = demoStep(s_demoLux, 200.0f, 800.0f, 8.0f, 3.0f);
    markOk(S_LIGHT);
  }

  if (currentConfig.sensor_enabled[S_WL] && sensorPinIsDemo(S_WL))
  {
    currentSensors.wl_percent = demoStep(s_demoWl, 40.0f, 90.0f, 0.5f, 1.0f);
    markOk(S_WL);
  }
}

static void markOk(SensorID id)
{
  currentSensors.last_ok_ms[id] = millis();
  currentSensors.last_err[id][0] = '\0';
}

static void markErr(SensorID id, const char *msg)
{
  strncpy(currentSensors.last_err[id], msg, sizeof(currentSensors.last_err[id]) - 1);
  currentSensors.last_err[id][sizeof(currentSensors.last_err[id]) - 1] = '\0';
}

// Disables a sensor in currentConfig (sensor_enabled[id] -> false), persists
// it, and logs why. Called only when a sensor fails every startup validation
// attempt. Deliberately leaves the sensor's pin assignment(s) untouched —
// sensor_enabled[] is the only on/off switch in this firmware, so the pin
// stays visible/correct in the UI for whenever the wiring gets fixed.
static void autoDisable(SensorID id, const char *name)
{
  currentConfig.sensor_enabled[id] = false;
  webLog(1, LOG_WARN, String(name) + " failed " + String(STARTUP_RETRY_COUNT) +
                           " startup read attempts — automatically disabled for this session. " +
                           "Re-enable from the Web UI once wiring is fixed and the device is rebooted.");
  state_save();
}

// ----------------------------------------------------------------------------
// I2C bus ownership
// ----------------------------------------------------------------------------
// The sensor task is the sole owner of the I2C bus on this board. It brings
// Wire up and performs a bounded presence probe for the BH1750 here, before
// ever handing control to the BH1750 library — sensor_lux_init() (in
// sensor_light.cpp) assumes Wire is already configured and just calls
// lightMeter.begin().
static bool bringUpI2CAndProbeLight()
{
  // Guard check: only bring up I2C and probe if the Light sensor is
  // actually enabled. sensor_enabled[] is the single on/off switch in this
  // firmware — pin values are never consulted to decide this.
  if (!currentConfig.sensor_enabled[S_LIGHT])
  {
    return false;
  }

  // Initialize the I2C bus using the dynamic pins from Web Doctor
  Wire.begin(currentConfig.pin_lux_sda, currentConfig.pin_lux_scl);

  // CRITICAL: bound every I2C transaction. Without this, a floating or
  // stuck SDA/SCL line can make the ESP32 Wire driver block forever
  // (no ACK, no clock-stretch timeout), which starves the sensor task
  // and trips the Core 1 task watchdog -> panic -> reboot. 1000ms is
  // generous for a healthy bus and still finite for a broken one.
  Wire.setTimeOut(1000);

  // Non-blocking presence probe BEFORE calling into the BH1750 library.
  // beginTransmission/endTransmission respects the timeout set above, so
  // this can now only ever take up to ~1s, never hang indefinitely.
  Wire.beginTransmission(BH1750_PROBE_ADDR);
  uint8_t probeResult = Wire.endTransmission();

  if (probeResult != 0)
  {
    // 0 = ACK received, sensor is there. Anything else (timeout, NACK,
    // bus error) means nothing responded on that address.
    webLog(1, LOG_ERR, "BH1750 not detected on I2C (SDA: " + String(currentConfig.pin_lux_sda) +
                            ", SCL: " + String(currentConfig.pin_lux_scl) +
                            "). Check wiring/pull-ups. Sensor disabled for this session.");
    return false;
  }

  return true;
}

// ----------------------------------------------------------------------------
// Startup validation
// ----------------------------------------------------------------------------
// For every sensor whose feature flag (sensor_enabled[]) is on, attempt up
// to STARTUP_RETRY_COUNT reads. The first successful read commits the
// sensor as "live" and moves on; if all attempts fail, the sensor is
// auto-disabled so the steady-state loop never has to deal with a sensor
// that was never actually there.
static void validateSensor(SensorID id, const char *name, bool (*readFn)())
{
  if (!currentConfig.sensor_enabled[id])
    return;

  bool progressLineShown = false;

  for (int attempt = 1; attempt <= STARTUP_RETRY_COUNT; attempt++)
  {
    // In-place Serial progress line — rewrites the same line for every
    // retry instead of scrolling one line per attempt (see webLogProgress()
    // in state.cpp). A sensor that passes on attempt 1 (the common case)
    // never shows this at all — it goes straight to the permanent result
    // line below, unchanged from before. progressLineShown (rather than
    // inferring "was it shown" from attempt > 1) keeps this correct even if
    // STARTUP_RETRY_COUNT is ever changed from its current value of 5.
    if (attempt > 1)
    {
      webLogProgress("  [CORE 1] " + String(name) + ": retrying (attempt " + String(attempt) + "/" + String(STARTUP_RETRY_COUNT) + ")...");
      progressLineShown = true;
    }

    if (readFn())
    {
      markOk(id);
      if (progressLineShown)
        webLogProgressDone();
      webLog(1, LOG_INFO, String(name) + " passed startup validation (attempt " + String(attempt) + "/" + String(STARTUP_RETRY_COUNT) + ").");
      return;
    }
    if (attempt < STARTUP_RETRY_COUNT)
      delay(STARTUP_RETRY_DELAY_MS);
  }

  if (progressLineShown)
    webLogProgressDone();
  markErr(id, "startup validation failed");
  autoDisable(id, name);
}

static void initAllSensors()
{
  // Demo mode: skip real hardware init/validation for every sensor currently
  // on the DEMO_MODE_PIN sentinel (sensorPinIsDemo()) — checked per-sensor
  // rather than one global branch, matching readAll()'s per-sensor check
  // below. In the common case (save_features swaps every sensor's pin
  // together) this behaves exactly like the old all-or-nothing branch: every
  // enabled sensor is in demo mode, so no real init/validation runs at all
  // and the log line below fires for the whole board.
  bool anyRealSensorEnabled = false;
  for (int i = 0; i < S_COUNT; i++)
  {
    if (currentConfig.sensor_enabled[i] && !sensorPinIsDemo((SensorID)i))
    {
      anyRealSensorEnabled = true;
      break;
    }
  }

  if (!anyRealSensorEnabled)
  {
    webLog(1, LOG_INFO, "Demo mode: every enabled sensor is simulated — skipping real hardware init.");
    readAllDemo();
    return;
  }

  if (!sensorPinIsDemo(S_DHT))
    sensor_dht_init();
  if (!sensorPinIsDemo(S_WTEMP))
    sensor_ds18b20_init();
  if (!sensorPinIsDemo(S_TDS))
    sensor_tds_init();
  if (!sensorPinIsDemo(S_PH))
    sensor_ph_init();
  if (!sensorPinIsDemo(S_WL))
    sensor_wl_init();

  // BH1750 is the one sensor with a reliable "is it actually wired up" probe
  // at init time (I2C ACK on the bus) — see bringUpI2CAndProbeLight() above,
  // which itself no-ops if the Light sensor is disabled. Skip the probe
  // entirely while Light is in demo mode (a real I2C probe against the
  // sentinel pins would only ever fail and auto-disable a sensor that was
  // never meant to touch hardware in the first place). Run it first when not
  // in demo mode; if nothing ACKed (and the sensor was enabled), disable it
  // immediately rather than letting the 5x startup-validation loop below
  // burn through retries on a device that was never there.
  if (!sensorPinIsDemo(S_LIGHT))
  {
    bool lightDetected = bringUpI2CAndProbeLight() && sensor_lux_init();
    if (!lightDetected && currentConfig.sensor_enabled[S_LIGHT])
    {
      currentConfig.sensor_enabled[S_LIGHT] = false;
      webLog(1, LOG_WARN, "BH1750 not found at boot — automatically disabled for this session. Re-enable from the Web UI once wiring is fixed and the device is rebooted.");
    }
  }

  // 5x startup validation for every sensor that survived init with its
  // feature flag still enabled — skipped per-sensor for anything still in
  // demo mode, same reasoning as the init calls above. Each capture-less
  // lambda adapts the sensor's real read signature to the bool()-returning
  // shape validateSensor() expects, discarding the value itself (only
  // pass/fail matters here — the steady-state readAll() below is what
  // populates currentSensors).
  if (!sensorPinIsDemo(S_DHT))
    validateSensor(S_DHT, "DHT22 (Air Temp/Humidity)", []() -> bool
                   { float t, h; return sensor_dht_read(t, h); });

  if (!sensorPinIsDemo(S_WTEMP))
    validateSensor(S_WTEMP, "DS18B20 (Water Temp)", []() -> bool
                   { float t; return sensor_ds18b20_read(t); });

  if (!sensorPinIsDemo(S_TDS))
    validateSensor(S_TDS, "TDS", []() -> bool
                   { float v; return sensor_tds_read(sensorPinIsDemo(S_WTEMP) ? 25.0f : currentSensors.water_temp_c, currentConfig.tds_k, v); });

  if (!sensorPinIsDemo(S_PH))
    validateSensor(S_PH, "pH", []() -> bool
                   { float v; return sensor_ph_read(currentConfig.ph_offset, currentConfig.ph_slope, v); });

  if (!sensorPinIsDemo(S_LIGHT))
    validateSensor(S_LIGHT, "BH1750 (Light)", []() -> bool
                   { float v; return sensor_lux_read(v); });

  if (!sensorPinIsDemo(S_WL))
    validateSensor(S_WL, "Water Level", []() -> bool
                   { float v; return sensor_wl_read(v); });

  // Any sensor that WAS in demo mode still needs its first simulated
  // reading populated now, same as the all-demo early-return path above did
  // for the whole board — otherwise a mixed real/demo boot would leave the
  // demo sensors' currentSensors fields at their zeroed startup value until
  // the next full sensorTaskLoop() cycle.
  readAllDemo();
}

static void readAll()
{
  // Demo mode: any sensor currently on the DEMO_MODE_PIN sentinel gets a
  // simulated reading instead of a real sensor_*_read() call — checked
  // per-sensor (sensorPinIsDemo()) rather than one global branch, so this
  // stays correct even if a sensor's pin was hand-set independently of the
  // usual save_features all-at-once swap. In the common case every enabled
  // sensor is in the same state, so this reads exactly like the old
  // all-or-nothing branch did.
  readAllDemo();

  // DHT (temp + humidity)
  if (currentConfig.sensor_enabled[S_DHT] && !sensorPinIsDemo(S_DHT))
  {
    float t = NAN, h = NAN;
    if (sensor_dht_read(t, h))
    {
      currentSensors.temp_c = t;
      currentSensors.humidity = h;
      currentSensors.vpd_kpa = computeVPD(t, h);
      markOk(S_DHT);
    }
    else
    {
      markErr(S_DHT, "read failed");
    }
  }

  // DS18B20 (water temp)
  if (currentConfig.sensor_enabled[S_WTEMP] && !sensorPinIsDemo(S_WTEMP))
  {
    float wt = NAN;
    if (sensor_ds18b20_read(wt))
    {
      currentSensors.water_temp_c = wt;
      markOk(S_WTEMP);
    }
    else
    {
      markErr(S_WTEMP, "read failed");
    }
  }

  // TDS (needs water temp for compensation)
  if (currentConfig.sensor_enabled[S_TDS] && !sensorPinIsDemo(S_TDS))
  {
    // Water temp compensation input: if Water Temp is currently faked
    // (independently toggled via save_sensor_demo — see command_handlers.cpp)
    // while TDS itself reads real hardware, currentSensors.water_temp_c is a
    // simulated value that would otherwise flow straight into readTDS()'s
    // compensation coefficient (sensor_tds.cpp) and silently skew the real
    // TDS reading by up to ~10% (compCoeff ranges 0.9-1.02 across demo's
    // 20-24°C simulated range) with no indication anywhere that the number
    // is partly synthetic. readTDS() already has a neutral-compensation
    // fallback for exactly this situation (currentWaterTemp <= 0.0 -> 25.0,
    // i.e. "don't compensate") — that guard just never fires for a fake
    // reading, since demo values are always a plausible positive number.
    // Route around it explicitly here instead of only when water temp is
    // missing entirely: pass the same neutral 25.0 whenever the input isn't
    // trustworthy, real hardware or not.
    bool waterTempIsFake = sensorPinIsDemo(S_WTEMP);
    float waterTempForComp = waterTempIsFake ? 25.0f : currentSensors.water_temp_c;

    float ppm = NAN;
    if (sensor_tds_read(waterTempForComp, currentConfig.tds_k, ppm))
    {
      currentSensors.tds_ppm = ppm;
      currentSensors.tds_comp_using_fake_water_temp = waterTempIsFake;
      markOk(S_TDS);
    }
    else
    {
      markErr(S_TDS, "read failed");
    }
  }

  // pH
  if (currentConfig.sensor_enabled[S_PH] && !sensorPinIsDemo(S_PH))
  {
    float ph = NAN;
    if (sensor_ph_read(currentConfig.ph_offset, currentConfig.ph_slope, ph))
    {
      currentSensors.ph_val = ph;
      markOk(S_PH);
    }
    else
    {
      markErr(S_PH, "read failed");
    }
  }

  // Lux
  if (currentConfig.sensor_enabled[S_LIGHT] && !sensorPinIsDemo(S_LIGHT))
  {
    float lx = NAN;
    if (sensor_lux_read(lx))
    {
      currentSensors.lux = lx;
      markOk(S_LIGHT);
    }
    else
    {
      markErr(S_LIGHT, "read failed");
    }
  }

  // Water level
  if (currentConfig.sensor_enabled[S_WL] && !sensorPinIsDemo(S_WL))
  {
    float pct = NAN;
    if (sensor_wl_read(pct))
    {
      currentSensors.wl_percent = pct;
      markOk(S_WL);
    }
    else
    {
      markErr(S_WL, "read failed");
    }
  }
}

void initSensorTask()
{
  if (s_initialized)
    return;
  s_initialized = true;
  printBootSection("SENSORS");
  webLog(1, LOG_INFO, "Sensor task started on core " + String(xPortGetCoreID()));

  // NOTE (Part 5.9): ledStatusInit() used to also be called here, duplicating
  // the call already made in HyGrow_IoT.ino's setup() (step 1c), before
  // either task starts. Adafruit_NeoPixel::begin() is idempotent so the
  // duplicate call wasn't breaking anything, but it's redundant and could
  // mislead a future reader into thinking the sensor task owns LED init.
  // The .ino's call is kept as the single owner since it already runs
  // before either task starts, which is the safer point.

  initAllSensors();
}

void sensorTaskLoop()
{
  static uint32_t lastRead = 0;
  uint32_t now = millis();
  if (s_kick || (now - lastRead) >= currentConfig.interval_read_ms)
  {
    s_kick = false;
    lastRead = now;
    readAll();

    // Mirror live hardware health on the WS2812 status LED right after the
    // read that just happened above. last_err[i] reflects only the most
    // recent read cycle for sensor i (markOk()/markErr() overwrite it every
    // time), so this always tracks the read we just did, not stale history.
    bool sensorErrors[S_COUNT] = {false};
    uint8_t enabledErrorCount = 0;
    for (int i = 0; i < S_COUNT; i++)
    {
      // Disabled/OFF sensors are skipped entirely — never counted, never
      // shown on the LED, exactly like the rest of this codebase already
      // treats them (autoDisable(), the Web UI, etc). Only a sensor that is
      // both enabled AND currently erroring counts toward anything below.
      if (!currentConfig.sensor_enabled[i])
      {
        continue;
      }
      if (currentSensors.last_err[i][0] != '\0')
      {
        sensorErrors[i] = true;
        enabledErrorCount++;
      }
    }

    if (enabledErrorCount >= 2)
    {
      // 2 or more enabled sensors failed this cycle — distinct fast white
      // strobe takes priority over the single-sensor color cycle so this
      // reads unambiguously as "multiple failures" at a glance.
      ledMultiSensorFailure();
    }
    else if (enabledErrorCount == 1)
    {
      ledCycleErrors(sensorErrors, currentConfig.sensor_enabled);
    }
    else
    {
      // No LED shown while healthy — only errors light it up now, so a
      // dark/off LED means "everything is fine," not "still booting" or
      // "no error yet checked." See ledStatusOff() in led_status.cpp.
      ledStatusOff();
    }
  }
  vTaskDelay(pdMS_TO_TICKS(50));
}

// NOTE (Part 5.6): sensorTaskFn(), sensor_task_start(), and sensor_task_kick()
// used to live here as an alternate task-creation entry point. They were
// never called from anywhere in the project — task_sensor.h didn't even
// declare them — and the real sensor task is started from HyGrow_IoT.ino's
// own sensorTaskWrapper()/xTaskCreatePinnedToCore() call. This looked like
// leftover scaffolding from an earlier iteration of the task-management
// architecture, and it was actively dangerous to keep: if anything ever
// called sensor_task_start() by mistake, it would spin up a SECOND FreeRTOS
// task pinned to Core 1 doing sensor init/reads concurrently with the real
// one, corrupting shared currentSensors/currentConfig state and likely
// double-driving the I2C bus and WS2812. Removed entirely — the .ino's
// sensorTaskWrapper() is the sole, correct entry point.
