"""Host checks for the real snapshot, JSON, Firestore and discovery code.

Only ESP32/Arduino hardware primitives are replaced. Requires g++ and the
ArduinoJson headers installed by the firmware's PlatformIO build.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ARDUINO = r"""
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
using String = std::string;
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()
inline size_t strlcpy(char* dst, const char* src, size_t size) {
    const auto len = strlen(src);
    if (size) { const auto n = len < size - 1 ? len : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return len;
}
struct ESPClass { uint64_t getEfuseMac() { return 0xaabbccddeeffULL; } };
inline ESPClass ESP;
"""

TEST = r"""
#include "src/core/telemetry.h"
#include "src/core/local_protocol.h"
#include "src/core/firebase_retry.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <atomic>

ConfigState currentConfig{};
SensorState currentSensors{};
VitalsState currentVitals{};
std::atomic<uint64_t> testNow{1000};
int64_t esp_timer_get_time() { return testNow.load() * 1000; }
uint32_t esp_random() { return 0x12345678; }
static void require(bool ok, const char* msg) {
    if (!ok) { std::cerr << msg << '\n'; std::exit(1); }
}
static String json(JsonVariantConst value) {
    String result; serializeJson(value, result); return result;
}
static bool discover(const String& value, DiscoveryRequest& out) {
    return parseDiscoveryRequest(reinterpret_cast<const uint8_t*>(value.data()), value.size(), out);
}
int main() {
    for (int i = 0; i < S_COUNT; ++i) currentConfig.sensor_enabled[i] = true;
    currentConfig.interval_read_ms = 2000;
    strlcpy(currentConfig.device_id, "grow-01", sizeof(currentConfig.device_id));
    telemetryInit();
    auto snap = telemetryRead();
    JsonDocument status;
    writeTelemetryJson(status.to<JsonObject>(), snap, 1000);
    require(status["sampleSequence"] == 0 && status["sampleAgeMs"].isNull(), "Boot must say waiting, without an invented sample");
    require(status["tds_ppm"].isNull() && status["sensorStatus"]["tds"]["state"] == "waiting", "Boot zeros must not look like real measurements");
    require(status["deviceId"] == "grow-01", "Keep configured cloud device identity");
    require(status["hardwareId"] == "esp32-aabbccddeeff", "Stable hardware identity needed for multiple devices");

    currentSensors.tds_ppm = 950.25f;
    currentSensors.temp_c = 24.5f; currentSensors.humidity = 62.0f;
    currentSensors.water_temp_c = 22; currentSensors.vpd_kpa = 1.15f;
    currentSensors.lux = 450; currentSensors.wl_percent = 65; currentSensors.ph_val = 6.2f;
    for (int i = 0; i < S_COUNT; ++i) currentSensors.last_ok_ms[i] = 1000;
    bool demo[S_COUNT]{}; demo[S_PH] = true;
    telemetryPublish(demo);
    snap = telemetryRead();
    // Mutating the producer's unfinished next cycle must not affect readers.
    currentSensors.tds_ppm = 1; currentSensors.temp_c = 99;
    writeTelemetryJson(status.to<JsonObject>(), telemetryRead(), 1500);
    require(status["tds_ppm"] == 950.25f && status["temp_c"] == 24.5f, "Readers saw an unfinished sensor cycle");
    require(status["sampleSequence"] == 1 && status["sampleAgeMs"] == 500, "Sample freshness is independent of request time");
    require(status["sensorStatus"]["ph"]["simulated"] == true, "Demo mode must travel with the sample");
    require(status["ph_voltage_mv"].isNull(), "A simulated pH sample must not expose a real calibration voltage");
    auto rawPh = snap;
    rawPh.simulated[S_PH] = false;
    rawPh.sensors.ph_voltage_mv = 1500;
    JsonDocument rawStatus;
    writeTelemetryJson(rawStatus.to<JsonObject>(), rawPh, 1500);
    require(rawStatus["ph_voltage_mv"] == 1500, "Calibration needs raw millivolts independent of clipped pH");

    JsonDocument dashboard, commit;
    writeDashboardTelemetry(dashboard.to<JsonObject>(), snap, 1500);
    writeTelemetryFirestore(commit.to<JsonObject>(), snap, 1500, "projects/test/databases/(default)/documents/devices/grow-01");
    require(dashboard["tds"] == status["tds_ppm"] && dashboard["hum"] == status["humidity"], "Dashboard aliases must equal canonical values");
    require(dashboard["s_ok"][S_TDS] == 1 && dashboard["type"] == "data", "Existing dashboard protocol must continue working");
    auto fields = commit["update"]["fields"];
    require(fields["tds_ppm"]["doubleValue"] == status["tds_ppm"], "Firestore and local values diverged");
    require(fields["sampleSequence"]["integerValue"] == "1", "Firestore integer wrapper incorrect");
    require(fields["sensorStatus"]["mapValue"]["fields"]["ph"]["mapValue"]["fields"]["simulated"]["booleanValue"] == true, "Firestore lost sensor provenance");
    require(commit["updateTransforms"][0]["setToServerValue"] == "REQUEST_TIME", "Keep server timestamp for remote stale detection");
    require(fields["lastUpdated"].isNull(), "Device clock must never masquerade as server time");

    snap.enabled[S_TDS] = false;
    strcpy(snap.sensors.last_err[S_DHT], "read failed");
    snap.sensors.last_ok_ms[S_WTEMP] = 0;
    snap.sensors.lux = INFINITY;
    writeTelemetryJson(status.to<JsonObject>(), snap, 1600);
    writeDashboardTelemetry(dashboard.to<JsonObject>(), snap, 1600);
    writeTelemetryFirestore(commit.to<JsonObject>(), snap, 1600, "test-path");
    require(status["tds_ppm"].isNull() && dashboard["tds"].isNull(), "Disabled sensor leaked stale value");
    require(status["temp_c"].isNull() && status["humidity"].isNull() && status["vpd_kpa"].isNull(), "DHT failure must also invalidate derived VPD");
    require(status["water_temp_c"].isNull() && status["lux"].isNull(), "Waiting or nonfinite values must be null");
    require(commit["update"]["fields"]["tds_ppm"]["nullValue"].isNull(), "Cloud must explicitly clear disabled fields");
    require(commit["update"]["fields"]["tds_ppm"].as<JsonObjectConst>().size() == 1, "Cloud null cannot mean omitted field");
    require(dashboard["s_ok"][S_TDS] == 0 && dashboard["s_ok"][S_DHT] == 2 && dashboard["s_ok"][S_WTEMP] == 3, "Sensor health codes changed");
    const JsonArrayConst mask = commit["updateMask"]["fieldPaths"].as<JsonArrayConst>();
    for (auto pair : status.as<JsonObjectConst>()) {
        bool masked = false;
        for (auto path : mask)
            if (strcmp(path.as<const char*>(), pair.key().c_str()) == 0) masked = true;
        require(masked, "Every canonical field must be in Firestore update mask");
    }
    const auto output = json(status.as<JsonVariantConst>());
    require(output.find("password") == String::npos && output.find("fb_api") == String::npos, "Read-only telemetry leaked configuration secrets");

    DiscoveryRequest req{};
    require(discover(R"({"type":"hygrow_discover","version":1,"requestId":"n-123"})", req), "Valid discovery request rejected");
    require(strcmp(req.requestId, "n-123") == 0, "Discovery must echo correlation ID");
    require(discover("Where is IoT?", req), "Human-readable discovery probe rejected");
    require(!discover(R"({"type":"other","version":1,"requestId":"n"})", req), "Unrelated UDP traffic accepted");
    require(!discover(R"({"type":"hygrow_discover","version":2,"requestId":"n"})", req), "Unknown discovery version accepted");
    require(!discover(R"({"type":"hygrow_discover","version":"1","requestId":"n"})", req), "String version should not coerce into protocol version");
    require(!discover(R"({"type":"hygrow_discover","version":1})", req), "JSON requests must carry a correlation ID");
    require(!discover(R"({"type":"hygrow_discover","version":1,"requestId":"x\n"})", req), "Unsafe correlation ID accepted");
    require(!discover(String(257, 'x'), req), "Oversized UDP request accepted");
    require(!discover("{}garbage", req), "Trailing data accepted");
    require(!discover(R"({"type":"hygrow_discover","version":1,"requestId":"n"}garbage)", req), "Valid JSON with trailing garbage accepted");
    require(!discover(R"({"type":"hygrow_discover","version":1,"requestId":"n\u0000x"})", req), "Embedded NUL correlation ID accepted");
    require(discover(R"({"type":"hygrow_discover","version":1,"requestId":"n","deviceId":"grow-01"})", req), "Device filter rejected");
    JsonDocument reply;
    writeDiscoveryReply(reply.to<JsonObject>(), req, snap, "192.168.0.42");
    require(reply["deviceId"] == "grow-01" && reply["requestId"] == "n", "Discovery identity/correlation missing");
    require(reply["statusUrl"] == "http://192.168.0.42/status" && reply["port"] == 80, "Discovery endpoint incorrect");
    require(localPeerAllowed(0xc0a80005, 0xc0a8002a, 0xffffff00), "Same subnet rejected");
    require(!localPeerAllowed(0xc0a80105, 0xc0a8002a, 0xffffff00), "Routed peer accepted");
    require(!localPeerAllowed(0xc0a800ff, 0xc0a8002a, 0xffffff00), "Broadcast source accepted");
    require(!localPeerAllowed(0xc0a80000, 0xc0a8002a, 0xffffff00), "Network source accepted");
    DiscoveryRateLimiter limit;
    require(limit.allow(0) && !limit.allow(50) && limit.allow(200), "Discovery rate limit incorrect");
    DiscoveryRateLimiter wrap;
    require(wrap.allow(0xfffffff0) && wrap.allow(200), "Rate limiter broken at millis wrap");
    for (int code : {-1, -11, 408, 429, 500, 503})
        require(!firebaseFailureIsPermanent(code), "Transient WAN failure must keep Firebase enabled");
    for (int code : {400, 401, 403, 404})
        require(firebaseFailureIsPermanent(code), "Permanent configuration error must be counted");

    // Stress cross-core publication with correlated fields.
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (int n = 1; n <= 10000; ++n) {
            currentSensors.temp_c = n; currentSensors.humidity = n;
            testNow = 1000 + n; telemetryPublish(demo);
        }
        done = true;
    });
    while (!done) {
        auto value = telemetryRead();
        if (value.sampleSequence > 1)
            require(value.sensors.temp_c == value.sensors.humidity, "Torn cross-core snapshot");
    }
    producer.join();
    std::cout << "Snapshot consistency, freshness, local/dashboard/cloud parity, nulls, discovery validation, rate limits and outage retry checks passed.\n";
}
"""

def main():
    compiler = shutil.which("g++")
    if not compiler:
        raise SystemExit("Install a host g++ compiler to run telemetry checks.")
    root = Path(__file__).resolve().parent.parent
    sources = ["telemetry.cpp", "telemetry_json.cpp", "local_protocol.cpp"]
    headers = ["state.h", "telemetry.h", "local_protocol.h", "firebase_retry.h"]
    missing = [name for name in sources + headers if not (root / "src/core" / name).exists()]
    if missing:
        raise SystemExit("Missing local telemetry implementation: " + ", ".join(missing))
    json_headers = root / ".pio/libdeps/esp32-s3-n16r8/ArduinoJson/src"
    if not json_headers.exists():
        raise SystemExit("Run the firmware PlatformIO build first to install ArduinoJson.")
    with tempfile.TemporaryDirectory(prefix="hygrow-telemetry-") as folder:
        work = Path(folder)
        core = work / "src/core"
        core.mkdir(parents=True)
        for name in sources + headers:
            shutil.copyfile(root / "src/core" / name, core / name)
        (work / "Arduino.h").write_text(ARDUINO)
        (work / "esp_timer.h").write_text("#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n")
        (work / "esp_system.h").write_text("#pragma once\n#include <cstdint>\nuint32_t esp_random();\n")
        enum = re.search(r"enum SensorID\s*\{[^}]+\};", (root / "config.h").read_text()).group()
        (work / "config.h").write_text("#pragma once\n" + enum)
        (work / "test.cpp").write_text(TEST)
        executable = work / "telemetry-test.exe"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-pthread",
                        "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                        "-I", str(work), "-I", str(json_headers),
                        *[str(core / name) for name in sources], str(work / "test.cpp"),
                        "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

if __name__ == "__main__":
    main()
