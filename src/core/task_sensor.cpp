#include "task_sensor.h"
#include "state.h"
#include "../sensors/sensors.h"
#include <math.h>

static TaskHandle_t s_sensorTask = nullptr;
static volatile bool s_kick = false;
static bool s_initialized = false;

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

static void readAll()
{
  // DHT (temp + humidity)
  if (currentConfig.sensor_enabled[S_DHT] && sensor_impl[S_DHT])
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
  if (currentConfig.sensor_enabled[S_WTEMP] && sensor_impl[S_WTEMP])
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
  if (currentConfig.sensor_enabled[S_TDS] && sensor_impl[S_TDS])
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
  if (currentConfig.sensor_enabled[S_PH] && sensor_impl[S_PH])
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
  if (currentConfig.sensor_enabled[S_LIGHT] && sensor_impl[S_LIGHT])
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
  if (currentConfig.sensor_enabled[S_WL] && sensor_impl[S_WL])
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
  sensors_init_all();
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
