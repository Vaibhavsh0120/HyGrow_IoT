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
struct AsyncWebSocketClient { int status() { return WS_CONNECTED; } void text(const String&); };
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
bool firebaseRequestPairing(uint32_t, const char*, const char*);
bool firebaseValidDeviceName(const char*);
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
inline void configTime(long, int, const char*, const char*, const char*) {}
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
extern bool fakeAutomatic, fakeAutoRegistered, fakeNameExists;
extern unsigned fakeSignups;
extern String fakePayload;
extern bool fakeTrusted;
extern std::function<void()> fakeCommitHook;
class HTTPClient {
    String url;
    int authStatus = 200;
public:
    void setConnectTimeout(unsigned) {}
    void setTimeout(unsigned) {}
    template<typename T> bool begin(T& client, const String& value) { url = value; fakeTrusted = client.trusted; return true; }
    void addHeader(const String&, const String&) {}
    int POST(const String& payload) {
        if (!fakeTrusted) return -1;
        if (url.find("identitytoolkit") != String::npos) {
            if (url.find("accounts:signUp") != String::npos) { ++fakeSignups; fakeAutoRegistered = true; return authStatus = 200; }
            ++fakeSignIns;
            return authStatus = fakeAutomatic && !fakeAutoRegistered ? 400 : fakeSignInCode;
        }
        ++fakeCommits; fakePayload = payload;
        if (fakeCommitCode == 200 && url.find("documents:commit") != String::npos &&
            payload.find("/devices/device-auth") != String::npos && payload.find("\"name\":{") != String::npos)
            fakeNameExists = true;
        if (fakeCommitHook) { auto hook = fakeCommitHook; fakeCommitHook = nullptr; hook(); }
        return fakeCommitCode;
    }
    int GET() {
        if (!fakeTrusted) return -1;
        if (url.find("mask.fieldPaths=name") != String::npos) return fakeNameExists ? 200 : 404;
        ++fakeGets;
        return url.find("/owners?") != String::npos ? 200 : 404;
    }
    String getString() {
        if (url.find("/owners?") != String::npos)
            return R"({"documents":[{"name":"projects/testproject/databases/(default)/documents/devices/device-auth/owners/user-a","fields":{"label":{"stringValue":"Farm user"},"blocked":{"booleanValue":true}}}]})";
        if (url.find("mask.fieldPaths=name") != String::npos)
            return R"({"fields":{"name":{"stringValue":"grow-01"}},"updateTime":"2026-11-15T12:00:00Z"})";
        if (url.find("identitytoolkit") != String::npos && authStatus == 400)
            return R"({"error":{"message":"INVALID_LOGIN_CREDENTIALS"}})";
        if (url.find("documents:commit") != String::npos)
            return R"({"writeResults":[{"transformResults":[{"timestampValue":"2026-11-15T12:00:00Z"}]}]})";
        return R"({"idToken":"test-token","localId":"device-auth","expiresIn":"3600"})";
    }
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
String fakePayload, fakeReply;
bool fakeAutomatic = false, fakeAutoRegistered = false, fakeNameExists = false;
unsigned fakeSignups = 0;
void AsyncWebSocketClient::text(const String& response) { fakeReply = response; }
bool fakeTrusted = false;
std::function<void()> fakeCommitHook;
FakeWS ws;
AsyncWebSocketClient client;
bool authed = true;
unsigned acks = 0, saves = 0;
bool lastAckOk = false;
AsyncWebSocketClient* FakeWS::client(uint32_t id) { return id == 13 ? &::client : nullptr; }
bool wsClientIsAuthed(uint32_t) { return authed; }
bool state_save() { ++saves; return true; }
bool state_save_device_id(const char*) { return true; }
bool state_pairing_identity(char (&code)[33], char (&hash)[65], bool create) {
    if (!create) return false;
    memset(code, 'a', 32); code[32] = 0;
    memset(hash, 'b', 64); hash[64] = 0;
    return true;
}
bool state_device_account(const char*, const char*, char (&email)[64], char (&password)[64], bool) {
    strcpy(email, "esp32-test@devices.hygrow");
    strcpy(password, "another-test-password");
    return true;
}
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
    require(fakeCommits == 1 && fakeSignIns == 1, "Cloud requests must verify a CA before sending credentials or tokens");
    firebaseUploadCycle();
    require(fakeCommits == 2 && fakeSignIns == 1, "Valid Firebase token was not cached");
    JsonDocument commit;
    const auto error = deserializeJson(commit, fakePayload, DeserializationOption::NestingLimit(20));
    require(!error, error.c_str());
    const auto fields = commit["writes"][0]["update"]["fields"];
    const auto tds = fields["sensors"]["mapValue"]["fields"]["tds"]["mapValue"]["fields"];
    require(tds["ppm"]["doubleValue"] == 950.25f, "Cloud worker bypassed shared snapshot");
    require(commit["writes"][0]["update"]["name"] == "projects/testproject/databases/(default)/documents/devices/device-auth",
            "Firestore document must be keyed by the Firebase Auth UID");
    require(fields["name"].isNull() && fields["device_auth_uid"].isNull() && fields["owner_uid"].isNull(),
            "Routine updates must leave the name and ownership alone");

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
    require(!currentConfig.firebase_enabled && saves == 0, "Cloud worker must defer NVS persistence to the network loop");
    firebaseNetworkLoop();
    require(saves == 1, "Network loop did not persist auto-disable once");

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
    authed = true;
    require(firebaseValidDeviceName("Greenhouse 1") && !firebaseValidDeviceName(""), "Device name validation failed");
    require(!firebaseRequestPairing(13, "block", "other/user-a"), "Owner path traversal was accepted");
    require(firebaseRequestPairing(13, "show", ""), "Permanent QR request was not queued");
    require(!firebaseRequestPairing(13, "owners", ""), "Two pairing requests were queued simultaneously");
    workerOnce(); firebaseNetworkLoop();
    JsonDocument registration, reply;
    require(!deserializeJson(registration, fakePayload) && !deserializeJson(reply, fakeReply), "Pairing registration/result failed");
    const auto write = registration["writes"][0];
    require(write["update"]["name"] == "projects/testproject/databases/(default)/documents/pairingDevices/device-auth" &&
            strlen(write["update"]["fields"]["codeHash"]["stringValue"] | "") == 64, "Pair registration must store the hash by device UID");
    require(reply["deviceUid"] == "device-auth" && strlen(reply["code"] | "") == 32 &&
            String(reply["code"].as<const char *>()).find('a') == 0, "Permanent QR reply missing device UID and code");
    require(firebaseRequestPairing(13, "owners", ""), "Owner list request was not queued");
    workerOnce(); firebaseNetworkLoop();
    require(!deserializeJson(reply, fakeReply) && reply["owners"][0]["uid"] == "user-a" &&
            reply["owners"][0]["blocked"] == true, "Owner list lost the blocked state");
    for (const char* action : {"block", "unblock"}) {
        require(firebaseRequestPairing(13, action, "user-a"), "Owner state request was not queued");
        workerOnce(); firebaseNetworkLoop();
        require(!deserializeJson(registration, fakePayload), "Owner state patch is invalid JSON");
        const auto change = registration["writes"][0];
        require(change["update"]["name"] == "projects/testproject/databases/(default)/documents/devices/device-auth/owners/user-a" &&
                change["updateMask"]["fieldPaths"][0] == "blocked" &&
                change["update"]["fields"]["blocked"]["booleanValue"] == (strcmp(action,"block") == 0), "Owner state patch targeted the wrong field");
    }
    require(firebaseRequestPairing(13, "rename", "Greenhouse 2"), "Rename request was not queued");
    workerOnce(); firebaseNetworkLoop();
    require(!deserializeJson(registration, fakePayload) && registration["writes"][0]["updateMask"]["fieldPaths"][0] == "name" &&
            strcmp(telemetryRead().deviceId, "Greenhouse 2") == 0, "Rename did not patch and refresh the displayed name");
    require(firebaseRequestPairing(13, "show", ""), "Could not re-open the permanent QR");
    workerOnce(); firebaseNetworkLoop();
    require(!deserializeJson(reply, fakeReply) && reply["deviceUid"] == "device-auth" &&
            strcmp(reply["code"] | "", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0, "Rename changed the permanent QR");

    fakeReply.clear();
    require(firebaseRequestPairing(13, "show", ""), "Final QR request was not queued");
    workerOnce(); authed = false; firebaseNetworkLoop();
    require(fakeReply.empty(), "Pairing secret was sent after administrator logout");

    const auto freshIdSignups = fakeSignups;
    fakeAutomatic = true;
    firebaseApplySettings("placeholder-key", "testproject", "", "", "devices");
    telemetryPublish(demo);
    firebaseUploadCycle();
    require(fakeSignups == freshIdSignups + 1, "New automatic account was not created");
    firebaseInvalidateToken();
    firebaseUploadCycle();
    require(fakeSignups == freshIdSignups + 1, "Automatic account should be reused");
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
        for name in sources + ["state.h", "telemetry.h", "firebase_retry.h", "firebase_tls.h"]:
            shutil.copyfile(root / "src/core" / name, core / name)
        (work / "Arduino.h").write_text(shared["ARDUINO"] + RTOS)
        (core / "task_network_internal.h").write_text(NETWORK)
        (core / "task_network.h").write_text('#pragma once\n#include "task_network_internal.h"\n')
        (work / "HTTPClient.h").write_text(HTTP)
        (work / "WiFi.h").write_text("#pragma once\nconstexpr int WL_CONNECTED=3;\nstruct FakeWiFi { int status() { return WL_CONNECTED; } };\ninline FakeWiFi WiFi;\n")
        (work / "WiFiClientSecure.h").write_text("#pragma once\nstruct WiFiClientSecure { bool trusted = false; void setInsecure() {} void setCACert(const char* cert) { trusted = cert && strstr(cert, \"BEGIN CERTIFICATE\"); } void setHandshakeTimeout(unsigned) {} };\n")
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
