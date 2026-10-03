#ifndef HYGROW_TELEMETRY_H
#define HYGROW_TELEMETRY_H

#include "state.h"
#include <ArduinoJson.h>

#define HYGROW_FIRMWARE_VERSION "3.1.4"
constexpr unsigned HYGROW_SCHEMA_VERSION = 1;

// No credentials or network-dependent fields in a sensor snapshot. Only
// the sensor task publishes it, after a complete read cycle. Consumers copy
// it under a short critical section, then serialize outside the lock.
struct TelemetrySnapshot
{
    SensorState sensors;
    bool enabled[S_COUNT];
    bool simulated[S_COUNT];
    char deviceId[32];
    char hardwareId[24];
    char bootId[17];
    uint64_t sampledUptimeMs;
    uint32_t sampleSequence;
    uint32_t readIntervalMs;
};

void telemetryInit(); // once, before the server or sensor task starts
void telemetryPublish(const bool simulated[S_COUNT]); // sensor task only
TelemetrySnapshot telemetryRead();
uint64_t telemetryUptimeMs();
uint8_t telemetrySensorStatus(const TelemetrySnapshot &snapshot, SensorID id);
void writeTelemetryJson(JsonObject out, const TelemetrySnapshot &snapshot, uint64_t nowMs);
void writeDashboardTelemetry(JsonObject out, const TelemetrySnapshot &snapshot, uint64_t nowMs);
void writeTelemetryFirestore(JsonObject write, const TelemetrySnapshot &snapshot,
                             uint64_t nowMs, const char *documentPath);

#endif
