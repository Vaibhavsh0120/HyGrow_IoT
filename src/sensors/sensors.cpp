/*
 * ============================================================================
 * sensors.cpp — Hardware Orchestrator
 * ============================================================================
 */
#include "sensors.h"
#include "../../config.h"
#include <math.h>

void sensors_init()
{
    init_wtemp();
    init_tds();
    init_dht();
    init_light();
    init_wl();
    init_ph();
}

void sensors_init_all()
{
    sensors_init();
}

void sensors_read_all(SensorData &data, const ConfigState &cfg)
{
    // 1. Water Temp (Read first for temperature compensation)
    if (cfg.sensor_enabled[S_WTEMP])
    {
        data.errors[S_WTEMP] = !read_wtemp(data.w_temp);
    }
    else
    {
        data.w_temp = 25.0; // Default for compensation
    }
    delay(200); // Prevent ADC ground loop

    // 2. TDS Sensor
    if (cfg.sensor_enabled[S_TDS])
    {
        data.tds_ppm = read_tds(data.w_temp);
        data.errors[S_TDS] = isnan(data.tds_ppm);
    }
    delay(200);

    // 3. DHT22
    if (cfg.sensor_enabled[S_DHT])
    {
        data.errors[S_DHT] = !read_dht(data.dht_temp, data.dht_hum);
    }
    delay(100); // Digital sensor, less delay needed

    // 4. BH1750 Light
    if (cfg.sensor_enabled[S_LIGHT])
    {
        data.light_lux = read_light();
        data.errors[S_LIGHT] = isnan(data.light_lux);
    }

    // 5. pH and water level
    if (cfg.sensor_enabled[S_PH])
    {
        data.ph_val = read_ph(data.w_temp);
        data.errors[S_PH] = isnan(data.ph_val);
    }
    if (cfg.sensor_enabled[S_WL])
    {
        data.wl_percent = read_wl();
        data.errors[S_WL] = isnan(data.wl_percent);
    }
}
