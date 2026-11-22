#include "telemetry.h"
#include <esp_system.h>
#include <esp_timer.h>

static portMUX_TYPE s_snapshotMux = portMUX_INITIALIZER_UNLOCKED;
static TelemetrySnapshot s_snapshot{};
static char s_hardwareId[24];
static char s_bootId[17];
static uint32_t s_sequence = 0; // owned by sensor task after init

uint64_t telemetryUptimeMs()
{
    return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

static TelemetrySnapshot makeSnapshot()
{
    TelemetrySnapshot next{};
    strlcpy(next.hardwareId, s_hardwareId, sizeof(next.hardwareId));
    strlcpy(next.bootId, s_bootId, sizeof(next.bootId));
    next.readIntervalMs = currentConfig.interval_read_ms;
    for (int i = 0; i < S_COUNT; ++i)
        next.enabled[i] = currentConfig.sensor_enabled[i];
    return next;
}

static void publishSnapshot(TelemetrySnapshot &next)
{
    portENTER_CRITICAL(&s_snapshotMux);
    strlcpy(next.deviceId, currentConfig.device_id[0] ? currentConfig.device_id : s_hardwareId,
            sizeof(next.deviceId));
    s_snapshot = next;
    portEXIT_CRITICAL(&s_snapshotMux);
}

void telemetrySetDeviceId(const char *id)
{
    portENTER_CRITICAL(&s_snapshotMux);
    strlcpy(currentConfig.device_id, id, sizeof(currentConfig.device_id));
    // Do not upload the measurement associated with the old ID again.
    strlcpy(s_snapshot.deviceId, id, sizeof(s_snapshot.deviceId));
    s_snapshot.sampleSequence = 0;
    s_snapshot.sampledUptimeMs = 0;
    portEXIT_CRITICAL(&s_snapshotMux);
}

void telemetryInit()
{
    snprintf(s_hardwareId, sizeof(s_hardwareId), "esp32-%012llx",
             static_cast<unsigned long long>(ESP.getEfuseMac() & 0xffffffffffffULL));
    snprintf(s_bootId, sizeof(s_bootId), "%08lx%08lx",
             static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
    s_sequence = 0;
    auto next = makeSnapshot();
    publishSnapshot(next);
}

void telemetryPublish(const bool simulated[S_COUNT])
{
    auto next = makeSnapshot();
    next.sensors = currentSensors;
    for (int i = 0; i < S_COUNT; ++i)
        next.simulated[i] = simulated[i];
    next.sampledUptimeMs = telemetryUptimeMs();
    next.sampleSequence = ++s_sequence;
    publishSnapshot(next);
}

TelemetrySnapshot telemetryRead()
{
    TelemetrySnapshot copy;
    portENTER_CRITICAL(&s_snapshotMux);
    copy = s_snapshot;
    portEXIT_CRITICAL(&s_snapshotMux);
    return copy;
}
