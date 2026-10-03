#include "telemetry.h"
#include <cmath>

uint8_t telemetrySensorStatus(const TelemetrySnapshot &s, SensorID id)
{
    if (!s.enabled[id]) return 0;
    if (!s.sampleSequence) return 3;
    if (s.sensors.last_err[id][0]) return 2;
    if (!s.sensors.last_ok_ms[id]) return 3;
    return 1;
}

static void reading(JsonObject out, const char *key, float value,
                    const TelemetrySnapshot &s, SensorID id)
{
    if (telemetrySensorStatus(s, id) == 1 && std::isfinite(value))
        out[key] = value;
    else
        out[key] = nullptr;
}

void writeTelemetryJson(JsonObject out, const TelemetrySnapshot &s, uint64_t nowMs)
{
    out["schemaVersion"] = HYGROW_SCHEMA_VERSION;
    out["deviceId"] = s.deviceId;
    out["hardwareId"] = s.hardwareId;
    out["bootId"] = s.bootId;
    out["firmwareVersion"] = HYGROW_FIRMWARE_VERSION;
    out["sampleSequence"] = s.sampleSequence;
    out["sampledUptimeMs"] = s.sampledUptimeMs;
    if (s.sampleSequence)
        out["sampleAgeMs"] = nowMs >= s.sampledUptimeMs ? nowMs - s.sampledUptimeMs : 0;
    else
        out["sampleAgeMs"] = nullptr;
    out["readIntervalMs"] = s.readIntervalMs;
    out["uptime_s"] = nowMs / 1000ULL;
    reading(out, "tds_ppm", s.sensors.tds_ppm, s, S_TDS);
    reading(out, "temp_c", s.sensors.temp_c, s, S_DHT);
    reading(out, "humidity", s.sensors.humidity, s, S_DHT);
    reading(out, "water_temp_c", s.sensors.water_temp_c, s, S_WTEMP);
    reading(out, "lux", s.sensors.lux, s, S_LIGHT);
    reading(out, "wl_percent", s.sensors.wl_percent, s, S_WL);
    reading(out, "ph_val", s.sensors.ph_val, s, S_PH);
    if (!s.simulated[S_PH]) reading(out, "ph_voltage_mv", s.sensors.ph_voltage_mv, s, S_PH);
    else out["ph_voltage_mv"] = nullptr;
    reading(out, "vpd_kpa", s.sensors.vpd_kpa, s, S_DHT);
    out["tds_comp_using_fake_water_temp"] = telemetrySensorStatus(s, S_TDS) == 1 &&
        !s.simulated[S_TDS] && s.sensors.tds_comp_using_fake_water_temp;

    static const char *names[S_COUNT] = {"water_level", "light", "tds", "dht", "ph", "water_temp"};
    static const char *states[] = {"disabled", "healthy", "failing", "waiting"};
    JsonObject health = out["sensorStatus"].to<JsonObject>();
    for (int i = 0; i < S_COUNT; ++i)
    {
        JsonObject sensor = health[names[i]].to<JsonObject>();
        sensor["enabled"] = s.enabled[i];
        sensor["simulated"] = s.simulated[i];
        sensor["state"] = states[telemetrySensorStatus(s, static_cast<SensorID>(i))];
    }
}

void writeDashboardTelemetry(JsonObject out, const TelemetrySnapshot &s, uint64_t nowMs)
{
    writeTelemetryJson(out, s, nowMs);
    out["type"] = "data";
    out["core_id_of_producer"] = 1;
    // Preserve the existing embedded dashboard without another sampling path.
    out["tds"] = out["tds_ppm"];
    out["temp"] = out["temp_c"];
    out["hum"] = out["humidity"];
    out["w_t"] = out["water_temp_c"];
    out["tds_fake_wt_comp"] = out["tds_comp_using_fake_water_temp"];
    JsonArray codes = out["s_ok"].to<JsonArray>();
    for (int i = 0; i < S_COUNT; ++i)
        codes.add(telemetrySensorStatus(s, static_cast<SensorID>(i)));
}

// Encode the same JSON in Firestore's REST typed Value representation.
static void firestoreValue(JsonObject out, JsonVariantConst value)
{
    if (value.isNull()) out["nullValue"] = nullptr;
    else if (value.is<bool>()) out["booleanValue"] = value.as<bool>();
    else if (value.is<const char *>()) out["stringValue"] = value.as<const char *>();
    else if (value.is<JsonObjectConst>())
    {
        JsonObject fields = out["mapValue"]["fields"].to<JsonObject>();
        for (JsonPairConst pair : value.as<JsonObjectConst>())
            firestoreValue(fields[pair.key().c_str()].to<JsonObject>(), pair.value());
    }
    else if (value.is<uint64_t>())
    {
        char digits[24];
        snprintf(digits, sizeof(digits), "%llu", static_cast<unsigned long long>(value.as<uint64_t>()));
        out["integerValue"] = digits;
    }
    else out["doubleValue"] = value.as<double>();
}

void writeTelemetryFirestore(JsonObject write, const TelemetrySnapshot &s,
                             uint64_t nowMs, const char *documentPath)
{
    JsonDocument canonical;
    writeTelemetryJson(canonical.to<JsonObject>(), s, nowMs);
    JsonArray paths = write["updateMask"]["fieldPaths"].to<JsonArray>();
    write["update"]["name"] = documentPath;
    JsonObject fields = write["update"]["fields"].to<JsonObject>();
    for (JsonPairConst pair : canonical.as<JsonObjectConst>())
    {
        paths.add(pair.key().c_str());
        firestoreValue(fields[pair.key().c_str()].to<JsonObject>(), pair.value());
    }
    paths.add("status");
    fields["status"]["stringValue"] = "Online";
    JsonObject transform = write["updateTransforms"].to<JsonArray>().add<JsonObject>();
    transform["fieldPath"] = "lastUpdated";
    transform["setToServerValue"] = "REQUEST_TIME";
}
