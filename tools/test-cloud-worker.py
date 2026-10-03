"""Exercise the production Firebase worker with simulated HTTP/RTOS hardware."""
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import tempfile

NETWORK = r"""
#pragma once
#include "state.h"
constexpr int WS_CONNECTED = 1;
struct AsyncWebSocketClient { int status() { return WS_CONNECTED; } };
struct FakeWS { AsyncWebSocketClient* client(uint32_t); };
extern FakeWS ws;
bool wsClientIsAuthed(uint32_t);
void broadcastConfig();
void sendCmdAck(AsyncWebSocketClient*, const String&, bool, const String& = "", bool = false);
struct FirebaseStatus { bool ready; uint32_t lastOkMs; char lastError[64]; };
FirebaseStatus firebaseReadStatus();
void firebaseApplySettings(const char*, const char*, const char*, const char*, const char*);
void firebaseSetEnabled(bool);
void firebaseInvalidateToken();
void firebaseStartWorker();
void firebaseUploadCycle();
void firebaseRequestUpload();
bool firebaseRequestTest(uint32_t);
void firebaseNetworkLoop();
"""

RTOS = r"""
using TaskHandle_t = void*;
constexpr int pdPASS = 1, pdTRUE = 1;
constexpr uint32_t portMAX_DELAY = 0xffffffff;
struct WorkerIdle {};
extern void (*fakeTask)(void*);
extern unsigned fakeNotifications, fakeWorkerCount;
inline int xTaskCreatePinnedToCore(void (*fn)(void*), const char*, unsigned, void*, int, TaskHandle_t* handle, int) {
    fakeTask = fn; *handle = reinterpret_cast<void*>(1); ++fakeWorkerCount; return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t) { ++fakeNotifications; }
inline unsigned ulTaskNotifyTake(int, uint32_t) {
    if (!fakeNotifications) throw WorkerIdle{};
    const auto n = fakeNotifications; fakeNotifications = 0; return n;
}
uint32_t millis();
inline String hostString(const char* value) { return value; }
inline String hostString(char* value) { return value; }
inline String hostString(const String& value) { return value; }
template<typename T> String hostString(T value) { return std::to_string(value); }
#define String(...) hostString(__VA_ARGS__)
"""

HTTP = r"""
#pragma once
#include <Arduino.h>
#include <functional>
extern int fakeSignInCode, fakeCommitCode;
extern unsigned fakeSignIns, fakeCommits, fakeGets;
extern String fakePayload;
extern std::function<void()> fakeCommitHook;
class HTTPClient {
    String url;
public:
    void setConnectTimeout(unsigned) {}
    void setTimeout(unsigned) {}
    template<typename T> bool begin(T&, const String& value) { url = value; return true; }
    void addHeader(const String&, const String&) {}
    int POST(const String& payload) {
        if (url.find("identitytoolkit") != String::npos) { ++fakeSignIns; return fakeSignInCode; }
        ++fakeCommits; fakePayload = payload;
        if (fakeCommitHook) { auto hook = fakeCommitHook; fakeCommitHook = nullptr; hook(); }
        return fakeCommitCode;
    }
    int GET() { ++fakeGets; return 404; }
    String getString() { return R"({"idToken":"test-token","expiresIn":"3600"})"; }
    void end() {}
};
"""

TEST = r"""
#include "src/core/telemetry.h"
#include "src/core/task_network_internal.h"
#include <functional>
#include <cstdlib>
#include <iostream>
ConfigState currentConfig{};
SensorState currentSensors{};
VitalsState currentVitals{};
uint64_t nowMs = 1000;
int64_t esp_timer_get_time() { return nowMs * 1000; }
uint32_t esp_random() { return 1; }
uint32_t millis() { return static_cast<uint32_t>(nowMs); }
void (*fakeTask)(void*) = nullptr;
unsigned fakeNotifications = 0, fakeWorkerCount = 0;
int fakeSignInCode = 200, fakeCommitCode = 200;
unsigned fakeSignIns = 0, fakeCommits = 0, fakeGets = 0;
String fakePayload;
std::function<void()> fakeCommitHook;
FakeWS ws;
AsyncWebSocketClient client;
bool authed = true;
unsigned acks = 0, saves = 0;
bool lastAckOk = false;
AsyncWebSocketClient* FakeWS::client(uint32_t id) { return id == 13 ? &::client : nullptr; }
bool wsClientIsAuthed(uint32_t) { return authed; }
bool state_save() { ++saves; return true; }
void webLog(uint8_t, uint8_t, const String&) {}
void broadcastConfig() {}
void sendCmdAck(AsyncWebSocketClient*, const String&, bool ok, const String&, bool) { ++acks; lastAckOk = ok; }
static void require(bool ok, const char* msg) { if (!ok) { std::cerr << msg << '\n'; std::exit(1); } }
static void workerOnce() { try { fakeTask(nullptr); } catch (const WorkerIdle&) {} }
static void provision() {
    firebaseApplySettings("placeholder-key", "testproject", "device@example.invalid", "placeholder-pass", "devices");
    firebaseSetEnabled(true);
}
int main() {
    currentConfig.interval_read_ms = 2000;
    strcpy(currentConfig.device_id, "grow-01");
    for (int i = 0; i < S_COUNT; ++i) { currentConfig.sensor_enabled[i] = true; currentSensors.last_ok_ms[i] = 1000; }
    currentSensors.tds_ppm = 950.25f;
    bool demo[S_COUNT]{};
    telemetryInit(); telemetryPublish(demo);
    provision();
    firebaseStartWorker(); firebaseStartWorker();
    require(fakeWorkerCount == 1, "More than one cloud worker created");
    firebaseRequestUpload(); firebaseRequestUpload(); firebaseRequestUpload();
    require(fakeCommits == 0 && fakeSignIns == 0, "Local loop performed synchronous HTTPS");
    workerOnce();
    require(fakeCommits == 1 && fakeSignIns == 1, "Pending uploads must coalesce into one latest-state write");
    firebaseUploadCycle();
    require(fakeCommits == 2 && fakeSignIns == 1, "Valid Firebase token was not cached");
    JsonDocument commit;
    require(!deserializeJson(commit, fakePayload, DeserializationOption::NestingLimit(16)), "Actual cloud request was not JSON");
    require(commit["writes"][0]["update"]["fields"]["tds_ppm"]["doubleValue"] == 950.25f, "Cloud worker bypassed shared snapshot");

    fakeCommitCode = 503;
    firebaseUploadCycle();
    const auto failures = fakeCommits;
    firebaseUploadCycle();
    require(fakeCommits == failures, "Outage retry did not back off");
    for (int i = 0; i < 8; ++i) { nowMs += 61000; firebaseUploadCycle(); }
    require(currentConfig.firebase_enabled && saves == 0, "WAN outage permanently disabled uploads or wrote NVS");
    fakeCommitCode = 200; nowMs += 61000; firebaseUploadCycle();
    require(firebaseReadStatus().ready, "Cloud did not recover after internet outage");

    fakeCommitCode = 403;
    for (int i = 0; i < 4; ++i) firebaseUploadCycle();
    require(currentConfig.firebase_enabled, "Permanent errors disabled too early");
    fakeCommitHook = [] { provision(); };
    firebaseUploadCycle();
    require(currentConfig.firebase_enabled, "Old in-flight credentials disabled newly saved settings");
    for (int i = 0; i < 5; ++i) firebaseUploadCycle();
    require(!currentConfig.firebase_enabled && saves == 1, "Repeated permissions errors did not disable once");

    provision(); fakeSignInCode = 503;
    for (int i = 0; i < 6; ++i) { nowMs += 61000; firebaseUploadCycle(); }
    require(currentConfig.firebase_enabled, "Sign-in service outage disabled uploads");
    fakeSignInCode = 200; fakeCommitCode = 200; nowMs += 61000;
    firebaseUploadCycle();
    require(firebaseReadStatus().ready, "Sign-in outage did not recover");

    const auto previousSignIns = fakeSignIns;
    require(firebaseRequestTest(13), "Manual cloud test not accepted");
    require(!firebaseRequestTest(13), "More than one manual test queued");
    require(fakeSignIns == previousSignIns, "Manual test blocked AsyncTCP callback with HTTPS");
    workerOnce(); firebaseNetworkLoop();
    require(acks == 1 && lastAckOk && fakeGets == 1, "Manual test did not return the existing acknowledgement");
    require(firebaseRequestTest(13), "Second manual cloud test not accepted");
    workerOnce(); authed = false; firebaseNetworkLoop();
    require(acks == 1, "Cloud test result sent to logged-out client");
    std::cout << "Cloud worker coalescing, cached auth, outage backoff/recovery, credential races, auto-disable and asynchronous manual tests passed.\n";
}
"""

def main():
    compiler = shutil.which("g++")
    if not compiler:
        raise SystemExit("Install a host g++ compiler to run cloud worker checks.")
    root = Path(__file__).resolve().parent.parent
    shared = runpy.run_path(str(root / "tools/test-local-telemetry.py"))
    sources = ["telemetry.cpp", "telemetry_json.cpp", "firebase.cpp"]
    with tempfile.TemporaryDirectory(prefix="hygrow-cloud-") as folder:
        work = Path(folder)
        core = work / "src/core"
        core.mkdir(parents=True)
        for name in sources + ["state.h", "telemetry.h", "firebase_retry.h"]:
            shutil.copyfile(root / "src/core" / name, core / name)
        (work / "Arduino.h").write_text(shared["ARDUINO"] + RTOS)
        (core / "task_network_internal.h").write_text(NETWORK)
        (core / "task_network.h").write_text('#pragma once\n#include "task_network_internal.h"\n')
        (work / "HTTPClient.h").write_text(HTTP)
        (work / "WiFi.h").write_text("#pragma once\nconstexpr int WL_CONNECTED=3;\nstruct FakeWiFi { int status() { return WL_CONNECTED; } };\ninline FakeWiFi WiFi;\n")
        (work / "WiFiClientSecure.h").write_text("#pragma once\nstruct WiFiClientSecure { void setInsecure() {} void setHandshakeTimeout(unsigned) {} };\n")
        (work / "esp_timer.h").write_text("#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n")
        (work / "esp_system.h").write_text("#pragma once\n#include <cstdint>\nuint32_t esp_random();\n")
        enum = re.search(r"enum SensorID\s*\{[^}]+\};", (root / "config.h").read_text()).group()
        (work / "config.h").write_text("#pragma once\nconstexpr int LOG_INFO=0, LOG_ERR=2;\n" + enum)
        (work / "test.cpp").write_text(TEST)
        executable = work / "cloud-test.exe"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-pthread",
                        "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                        "-I", str(work), "-I", str(root / ".pio/libdeps/esp32-s3-n16r8/ArduinoJson/src"),
                        *[str(core / name) for name in sources], str(work / "test.cpp"),
                        "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

if __name__ == "__main__":
    main()
