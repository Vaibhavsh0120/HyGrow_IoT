/*
 * ============================================================================
 * task_sensor.cpp — Sensor Task (Core 1)
 * ============================================================================
 * Single source of truth for all sensor hardware interaction. Owns:
 *   1. Boot-time init of every sensor (including the BH1750 I2C presence
 *      probe, which used to live in the now-deleted sensors.cpp).
 *   2. Startup validation: each enabled sensor gets up to 5 read attempts
 *      before the task commits to it being "live". A sensor that fails all
 *      5 boot attempts is auto-disabled (pin -> -1) and persisted, so a
 *      genuinely unwired/dead sensor doesn't spam "read failed" into the
 *      web terminal forever.
 *   3. The steady-state read loop.
 * ============================================================================
 */
#include "task_sensor.h"
#include "state.h"
#include <math.h>

static TaskHandle_t s_sensorTask = nullptr;
static volatile bool s_kick = false;
static bool s_initialized = false;

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

// Disables a sensor in currentConfig (pin(s) -> -1), persists it, and logs
// why. Called only when a sensor fails every startup validation attempt.
static void autoDisable(SensorID id, const char *name)
{
  switch (id)
  {
  case S_DHT:
    currentConfig.pin_dht = -1;
    break;
  case S_WTEMP:
    currentConfig.pin_ds18b20 = -1;
    break;
  case S_TDS:
    currentConfig.pin_tds = -1;
    break;
  case S_PH:
    currentConfig.pin_ph = -1;
    break;
  case S_LIGHT:
    currentConfig.pin_lux_sda = -1;
    currentConfig.pin_lux_scl = -1;
    break;
  case S_WL:
    currentConfig.pin_wl = -1;
    currentConfig.pin_wl_power = -1;
    break;
  default:
    break;
  }

  currentConfig.sensor_enabled[id] = false;
  webLog(1, LOG_WARN, String(name) + " failed " + String(STARTUP_RETRY_COUNT) +
                           " startup read attempts — automatically disabled for this session. " +
                           "Re-enable from the Web UI once wiring is fixed and the device is rebooted.");
  state_save();
}

// ----------------------------------------------------------------------------
// Startup validation
// ----------------------------------------------------------------------------
// For every sensor whose pin(s) are currently assigned (>= 0) AND whose
// feature flag is enabled, attempt up to STARTUP_RETRY_COUNT reads. The
// first successful read commits the sensor as "live" and moves on; if all
// attempts fail, the sensor is auto-disabled so the steady-state loop never
// has to deal with a sensor that was never actually there.
static void validateSensor(SensorID id, const char *name, bool (*readFn)())
{
  if (!currentConfig.sensor_enabled[id])
    return;

  for (int attempt = 1; attempt <= STARTUP_RETRY_COUNT; attempt++)
  {
    if (readFn())
    {
      markOk(id);
      webLog(1, LOG_INFO, String(name) + " passed startup validation (attempt " + String(attempt) + "/" + String(STARTUP_RETRY_COUNT) + ").");
      return;
    }
    if (attempt < STARTUP_RETRY_COUNT)
      delay(STARTUP_RETRY_DELAY_MS);
  }

  markErr(id, "startup validation failed");
  autoDisable(id, name);
}

static void initAllSensors()
{
  // BH1750 is the one sensor with a reliable "is it actually wired up" probe
  // at init time (I2C ACK on the bus) — see sensor_light.cpp. Run that probe
  // first; if nothing ACKed, disable it immediately rather than letting the
  // 5x startup-validation loop below burn through retries on a device that
  // was never there.
  sensor_dht_init();
  sensor_ds18b20_init();
  sensor_tds_init();
  sensor_ph_init();
  sensor_wl_init();

  bool lightDetected = sensor_lux_init();
  if (!lightDetected && currentConfig.sensor_enabled[S_LIGHT])
  {
    currentConfig.sensor_enabled[S_LIGHT] = false;
    webLog(1, LOG_WARN, "BH1750 not found at boot — automatically disabled for this session. Re-enable from the Web UI once wiring is fixed and the device is rebooted.");
  }

  // 5x startup validation for every sensor that survived init with its
  // feature flag still enabled. Each capture-less lambda adapts the
  // sensor's real read signature to the bool()-returning shape validateSensor()
  // expects, discarding the value itself (only pass/fail matters here —
  // the steady-state readAll() below is what populates currentSensors).
  validateSensor(S_DHT, "DHT22 (Air Temp/Humidity)", []() -> bool
                 { float t, h; return sensor_dht_read(t, h); });

  validateSensor(S_WTEMP, "DS18B20 (Water Temp)", []() -> bool
                 { float t; return sensor_ds18b20_read(t); });

  validateSensor(S_TDS, "TDS", []() -> bool
                 { float v; return sensor_tds_read(currentSensors.water_temp_c, currentConfig.tds_k, v); });

  validateSensor(S_PH, "pH", []() -> bool
                 { float v; return sensor_ph_read(currentConfig.ph_offset, currentConfig.ph_slope, v); });

  validateSensor(S_LIGHT, "BH1750 (Light)", []() -> bool
                 { float v; return sensor_lux_read(v); });

  validateSensor(S_WL, "Water Level", []() -> bool
                 { float v; return sensor_wl_read(v); });
}

static void readAll()
{
  // DHT (temp + humidity)
  if (currentConfig.sensor_enabled[S_DHT])
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
  if (currentConfig.sensor_enabled[S_WTEMP])
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
  if (currentConfig.sensor_enabled[S_TDS])
  {
    float ppm = NAN;
    if (sensor_tds_read(currentSensors.water_temp_c, currentConfig.tds_k, ppm))
    {
      currentSensors.tds_ppm = ppm;
      markOk(S_TDS);
    }
    else
    {
      markErr(S_TDS, "read failed");
    }
  }

  // pH
  if (currentConfig.sensor_enabled[S_PH])
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
  if (currentConfig.sensor_enabled[S_LIGHT])
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
  if (currentConfig.sensor_enabled[S_WL])
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
  webLog(1, LOG_INFO, "Sensor task started on core " + String(xPortGetCoreID()));
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
  }
  vTaskDelay(pdMS_TO_TICKS(50));
}

static void sensorTaskFn(void *)
{
  initSensorTask();
  for (;;)
  {
    sensorTaskLoop();
  }
}

void sensor_task_start()
{
  if (s_sensorTask)
    return;
  xTaskCreatePinnedToCore(
      sensorTaskFn,
      "sensorTask",
      8192,
      nullptr,
      1, // priority
      &s_sensorTask,
      1 // Core 1
  );
}

void sensor_task_kick()
{
  s_kick = true;
}
