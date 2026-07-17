// -------------------------------------------------------------
// WARNING: CODE EXPLICITLY REMOVED BY USER REQUEST
// If this sensor is toggled ON in the Web UI, an
// "INCOMPLETE CODE" banner will appear.
// -------------------------------------------------------------
#include <Arduino.h>

void init_wl() { /* DO NOTHING */ }
float read_wl() { return 0.0; }

bool sensor_wl_read(float &percent)
{
    percent = read_wl();
    return !isnan(percent);
}
