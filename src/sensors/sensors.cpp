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

    // BH1750 is the one sensor with a reliable "is it actually wired up"
    // probe at init time (see sensor_light.cpp). If nothing ACKed on the
    // I2C bus, disable it in the shared config now rather than letting
    // sensors_read_all() call into it every single cycle for a device
    // that isn't there.
    bool lightDetected = init_light();
    if (!lightDetected && currentConfig.sensor_enabled[S_LIGHT])
    {
        currentConfig.sensor_enabled[S_LIGHT] = false;
        webLog(1, LOG_WARN, "BH1750 not found at boot — automatically disabled for this session. Re-enable from the Web UI once wiring is fixed and the device is rebooted.");
    }

    init_wl();
    init_ph();
}

void sensors_init_all()
{
    sensors_init();
}
