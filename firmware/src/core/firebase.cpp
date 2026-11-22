// ----------------------------------------------------------------------------
// firebase.cpp — Firebase / Firestore device-state upload (Part 5.3 / Part 6).
// ----------------------------------------------------------------------------
// Split out of the original task_network.cpp (see task_network_internal.h
// for the full map of the split).
//
// REST uploads run on a dedicated, notification-driven worker. Local HTTP,
// UDP discovery and dashboard telemetry never wait for cloud TLS requests.
// Pending requests coalesce into one latest-state upload; no history queue
// grows during an outage. Firebase settings are copied under a short lock,
// so changing credentials cannot corrupt an in-flight request.
//
// ---------------------------------------------------------------------------
// Document path: devices/{Firebase Auth UID} (collection configurable).
// The firmware replaces device, sample and sensors on each successful upload
// and updates the server-side heartbeat. Firestore provisioning owns the
// owner link documents. Names are initialized once and edited separately.
#include "task_network_internal.h"
#include "task_network.h"
#include "state.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "telemetry.h"
#include "firebase_retry.h"
#include "firebase_tls.h"
#include <new>
#include <time.h>

static String s_fbIdToken;
static String s_fbDeviceUid;
static uint32_t s_fbTokenExpiryMs = 0; // millis() timestamp after which the cached token is considered stale

// Only repeated permanent 4xx configuration/permission errors auto-disable.
// WAN outages, socket timeouts, 408/429 and 5xx keep the user's toggle ON.
#define FIREBASE_MAX_CONSECUTIVE_FAILURES 5
static uint8_t s_fbConsecutiveFailures = 0; // worker-owned
static int s_signInFailureCode = 0; // worker-owned
static TaskHandle_t s_uploadWorker = nullptr;
static portMUX_TYPE s_settingsMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_settingsGeneration = 1;
static uint32_t s_workerGeneration = 0;
static uint32_t s_retryDelayMs = 0;
static uint64_t s_nextAttemptMs = 0;
static bool s_cloudBusy = false;
static uint32_t s_pendingDisableGeneration = 0;
static uint32_t s_testClientId = 0;
static uint32_t s_resultClientId = 0;
static bool s_resultOk = false;
static char s_resultError[160]{};

enum class PairAction : uint8_t { Show, Owners, Block, Unblock, ReadName, Rename };

struct PairRequest
{
    uint32_t clientId;
    PairAction action;
    char argument[512];
};

struct PairReply
{
    uint32_t clientId;
    bool ok;
    String payload;
};

static PairRequest s_pairRequest{};
static PairReply *s_pairReply = nullptr;
static char s_pairingCode[33]{}, s_pairingHash[65]{};
static char s_liveCloudName[32]{}, s_cloudTimestamp[40]{};
static uint32_t s_cloudNameGeneration = 0;
static uint64_t s_nextNameCheckMs = 0;

static bool safePathId(const char *id, size_t limit)
{
    size_t length = 0;
    for (; id[length]; ++length)
    {
        const char c = id[length];
        if (length >= limit || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                 (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return length != 0;
}

bool firebaseValidDeviceName(const char *name)
{
    const size_t size = strlen(name);
    if (!size || size > 31 || name[0] == ' ' || name[size - 1] == ' ') return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p; ++p)
        if (*p < 0x20 || *p == 0x7f) return false;
    return true;
}

struct FirebaseSettings
{
    char fb_api_key[128], fb_project[64], fb_email[64], fb_pass[64], fb_collection[32];
    bool firebase_enabled;
};

static FirebaseSettings firebaseReadConfig(uint32_t *generation = nullptr)
{
    FirebaseSettings copy;
    portENTER_CRITICAL(&s_settingsMux);
    strlcpy(copy.fb_api_key, currentConfig.fb_api_key, sizeof(copy.fb_api_key));
    strlcpy(copy.fb_project, currentConfig.fb_project, sizeof(copy.fb_project));
    strlcpy(copy.fb_email, currentConfig.fb_email, sizeof(copy.fb_email));
    strlcpy(copy.fb_pass, currentConfig.fb_pass, sizeof(copy.fb_pass));
    strlcpy(copy.fb_collection, currentConfig.fb_collection, sizeof(copy.fb_collection));
    copy.firebase_enabled = currentConfig.firebase_enabled;
    if (generation) *generation = s_settingsGeneration;
    portEXIT_CRITICAL(&s_settingsMux);
    return copy;
}

static bool resolveAutomaticAccount(FirebaseSettings &settings, bool create)
{
    return state_device_account(settings.fb_project, telemetryRead().hardwareId,
                                settings.fb_email, settings.fb_pass, create);
}

String firebaseSavedPassword()
{
    auto settings = firebaseReadConfig();
    if (!settings.fb_email[0] && !resolveAutomaticAccount(settings, false)) return "";
    return String(settings.fb_pass);
}

String firebaseAutomaticEmail()
{
    auto settings = firebaseReadConfig();
    if (settings.fb_email[0] || !resolveAutomaticAccount(settings, false)) return "";
    return String(settings.fb_email);
}

void firebaseSetEnabled(bool enabled)
{
    portENTER_CRITICAL(&s_settingsMux);
    currentConfig.firebase_enabled = enabled;
    portEXIT_CRITICAL(&s_settingsMux);
}

FirebaseStatus firebaseReadStatus()
{
    FirebaseStatus copy;
    portENTER_CRITICAL(&s_settingsMux);
    copy.ready = currentConfig.firebase_enabled && currentVitals.firebase_ready;
    copy.lastOkMs = currentVitals.firebase_last_ok_ms;
    memcpy(copy.lastError, currentVitals.firebase_last_error, sizeof(copy.lastError));
    portEXIT_CRITICAL(&s_settingsMux);
    return copy;
}

static void firebaseUpdateStatus(bool ready, const char *error = "")
{
    portENTER_CRITICAL(&s_settingsMux);
    currentVitals.firebase_ready = ready;
    if (ready) currentVitals.firebase_last_ok_ms = millis();
    strlcpy(currentVitals.firebase_last_error, error, sizeof(currentVitals.firebase_last_error));
    portEXIT_CRITICAL(&s_settingsMux);
}

void firebaseApplySettings(const char *api, const char *project, const char *email,
                           const char *password, const char *collection)
{
    portENTER_CRITICAL(&s_settingsMux);
    strlcpy(currentConfig.fb_api_key, api, sizeof(currentConfig.fb_api_key));
    strlcpy(currentConfig.fb_project, project, sizeof(currentConfig.fb_project));
    strlcpy(currentConfig.fb_email, email, sizeof(currentConfig.fb_email));
    if (password && password[0])
        strlcpy(currentConfig.fb_pass, password, sizeof(currentConfig.fb_pass));
    strlcpy(currentConfig.fb_collection, collection, sizeof(currentConfig.fb_collection));
    ++s_settingsGeneration;
    portEXIT_CRITICAL(&s_settingsMux);
}

void firebaseInvalidateToken()
{
    // The worker is the only task allowed to mutate its String/token state.
    portENTER_CRITICAL(&s_settingsMux);
    ++s_settingsGeneration;
    portEXIT_CRITICAL(&s_settingsMux);
}

void firebaseResetFailureCount()
{
    firebaseInvalidateToken();
}

// Start SNTP without blocking local telemetry; fail closed until time is set.
static bool firebaseClockReady()
{
    static bool started = false;
    if (!started)
    {
        configTime(0, 0, "time.google.com", "pool.ntp.org", "time.nist.gov");
        started = true;
    }
    return time(nullptr) >= 1735689600; // 2025-01-01, reject unset epoch
}

struct FirebaseIdentity
{
    String token, uid, serviceError, error;
    int status = 0;
    unsigned expiresIn = 3600;
};

static FirebaseIdentity firebaseAuthRequest(const FirebaseSettings &settings, bool signup)
{
    FirebaseIdentity result;
    WiFiClientSecure client;
    client.setCACert(FIREBASE_ROOT_CA);
    client.setHandshakeTimeout(5);
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    const String url = "https://identitytoolkit.googleapis.com/v1/accounts:" +
        String(signup ? "signUp" : "signInWithPassword") + "?key=" + settings.fb_api_key;
    if (!http.begin(client, url))
    {
        result.error = "Could not start Firebase authentication.";
        return result;
    }
    http.addHeader("Content-Type", "application/json");
    JsonDocument request;
    request["email"] = settings.fb_email;
    request["password"] = settings.fb_pass;
    request["returnSecureToken"] = true;
    String payload;
    serializeJson(request, payload);
    result.status = http.POST(payload);

    JsonDocument reply;
    const auto parsed = deserializeJson(reply, http.getString());
    http.end();
    if (result.status == 200)
    {
        const char *uid = reply["localId"] | "";
        const char *token = reply["idToken"] | "";
        if (!parsed && token[0] && safePathId(uid, 128))
        {
            result.uid = uid;
            result.token = token;
            const unsigned seconds = strtoul(reply["expiresIn"] | "3600", nullptr, 10);
            if (seconds > 0 && seconds <= 86400) result.expiresIn = seconds;
        }
        else result.error = "Firebase returned an invalid identity.";
    }
    else
    {
        result.serviceError = reply["error"]["message"] | "";
        result.error = String(signup ? "Device account setup: " : "Firebase sign-in: ") +
            (result.serviceError.length() ? result.serviceError : "HTTP " + String(result.status));
    }
    return result;
}

static FirebaseIdentity firebaseAuthenticate(FirebaseSettings settings)
{
    FirebaseIdentity result;
    if (!settings.fb_api_key[0] || !settings.fb_project[0])
    {
        result.status = 400;
        result.error = "Enter the Firebase Project ID and Web API Key.";
        return result;
    }
    if (!firebaseClockReady())
    {
        result.error = "Waiting for network time for TLS validation.";
        return result;
    }
    const bool automatic = !settings.fb_email[0];
    if (automatic && !resolveAutomaticAccount(settings, true))
    {
        result.error = "Could not save the device's Firebase account.";
        return result;
    }
    if (!settings.fb_pass[0])
    {
        result.status = 400;
        result.error = "Enter the Firebase password or leave Email blank for automatic setup.";
        return result;
    }
    result = firebaseAuthRequest(settings, false);
    if (automatic && result.status == 400 &&
        (result.serviceError == "EMAIL_NOT_FOUND" || result.serviceError == "INVALID_LOGIN_CREDENTIALS"))
        result = firebaseAuthRequest(settings, true);
    return result;
}

static String firestoreDocumentPath(const FirebaseSettings &settings, const String &uid)
{
    const String collection = settings.fb_collection[0] ? settings.fb_collection : "devices";
    return "projects/" + String(settings.fb_project) + "/databases/(default)/documents/" + collection + "/" + uid;
}

// A manual test refreshes authentication independently of the cached worker token.
bool firebaseTestConnection(String &errorOut)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        errorOut = "Device is not connected to Wi-Fi.";
        return false;
    }
    const auto settings = firebaseReadConfig();
    const auto identity = firebaseAuthenticate(settings);
    if (!identity.token.length())
    {
        errorOut = identity.error;
        return false;
    }
    WiFiClientSecure client;
    client.setCACert(FIREBASE_ROOT_CA);
    client.setHandshakeTimeout(5);
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    if (!http.begin(client, "https://firestore.googleapis.com/v1/" + firestoreDocumentPath(settings, identity.uid)))
    {
        errorOut = "Signed in, but could not start the Firestore check.";
        return false;
    }
    http.addHeader("Authorization", "Bearer " + identity.token);
    const int status = http.GET();
    http.end();
    if (status == 200 || status == 404) return true;
    errorOut = "Signed in, but Firestore check returned HTTP " + String(status) + ".";
    return false;
}

static bool firebaseEnsureIdToken(const FirebaseSettings &settings)
{
    s_signInFailureCode = 0;
    if (s_fbIdToken.length() && (int32_t)(millis() - s_fbTokenExpiryMs) < 0) return true;

    const auto identity = firebaseAuthenticate(settings);
    s_signInFailureCode = identity.status;
    if (!identity.token.length())
    {
        firebaseUpdateStatus(false, identity.error.c_str());
        return false;
    }
    s_fbIdToken = identity.token;
    s_fbDeviceUid = identity.uid;
    s_fbTokenExpiryMs = millis() + static_cast<uint32_t>(identity.expiresIn * 800UL);
    return true;
}

// Failures from older credentials cannot disable freshly saved settings.
static void firebaseRegisterFailure(int code, uint32_t generation)
{
    if (!firebaseFailureIsPermanent(code))
    {
        s_fbConsecutiveFailures = 0;
        s_retryDelayMs = s_retryDelayMs ? (s_retryDelayMs < 30000 ? s_retryDelayMs * 2 : 60000) : 5000;
        s_nextAttemptMs = telemetryUptimeMs() + s_retryDelayMs;
        return;
    }
    bool disabled = false;
    portENTER_CRITICAL(&s_settingsMux);
    if (generation == s_settingsGeneration)
    {
        if (s_fbConsecutiveFailures < 255) ++s_fbConsecutiveFailures;
        if (s_fbConsecutiveFailures >= FIREBASE_MAX_CONSECUTIVE_FAILURES && currentConfig.firebase_enabled)
        {
            currentConfig.firebase_enabled = false;
            disabled = true;
            s_pendingDisableGeneration = generation;
        }
    }
    portEXIT_CRITICAL(&s_settingsMux);
    if (disabled)
    {
        webLog(0, LOG_ERR, "Firebase has 5 permanent configuration/permission failures. Fix Cloud Provisioning, then re-enable uploads.");
    }
}

static void firebaseSyncWorkerGeneration(uint32_t generation)
{
    if (generation != s_workerGeneration)
    {
        s_fbIdToken = "";
        s_fbTokenExpiryMs = 0;
        s_fbConsecutiveFailures = 0;
        s_retryDelayMs = 0;
        s_nextAttemptMs = 0;
        s_nextNameCheckMs = 0;
        portENTER_CRITICAL(&s_settingsMux);
        s_cloudTimestamp[0] = '\0';
        portEXIT_CRITICAL(&s_settingsMux);
        s_workerGeneration = generation;
    }
}

struct CloudName
{
    String name, updateTime;
    int status = 0;
};

static CloudName firebaseReadCloudName(const String &documentPath)
{
    CloudName current;
    WiFiClientSecure client;
    client.setCACert(FIREBASE_ROOT_CA);
    client.setHandshakeTimeout(5);
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    if (!http.begin(client, "https://firestore.googleapis.com/v1/" + documentPath + "?mask.fieldPaths=name"))
        return current;
    http.addHeader("Authorization", "Bearer " + s_fbIdToken);
    current.status = http.GET();
    if (current.status == 200)
    {
        JsonDocument response;
        if (deserializeJson(response, http.getString())) current.status = 0;
        else
        {
            current.name = response["fields"]["name"]["stringValue"] | "";
            current.updateTime = response["updateTime"] | "";
        }
    }
    http.end();
    return current;
}

static void rememberCloudName(const char *name, uint32_t generation)
{
    if (!firebaseValidDeviceName(name)) return;
    portENTER_CRITICAL(&s_settingsMux);
    if (generation == s_settingsGeneration)
    {
        strlcpy(s_liveCloudName, name, sizeof(s_liveCloudName));
        s_cloudNameGeneration = generation;
    }
    portEXIT_CRITICAL(&s_settingsMux);
}

String firebaseLastUpdated()
{
    char stamp[sizeof(s_cloudTimestamp)];
    portENTER_CRITICAL(&s_settingsMux);
    memcpy(stamp, s_cloudTimestamp, sizeof(stamp));
    portEXIT_CRITICAL(&s_settingsMux);
    return String(stamp);
}

// Called exclusively by the cloud worker. Upload the newest complete sensor
// cycle after sign-in using the Firestore sensor groups.
void firebaseUploadCycle()
{
    uint32_t generation;
    const auto settings = firebaseReadConfig(&generation);
    if (!settings.firebase_enabled || WiFi.status() != WL_CONNECTED ||
        !settings.fb_project[0] || !settings.fb_api_key[0]) return;

    firebaseSyncWorkerGeneration(generation);
    if (telemetryUptimeMs() < s_nextAttemptMs) return;
    if (!firebaseEnsureIdToken(settings))
    {
        const auto status = firebaseReadStatus();
        webLog(0, LOG_ERR, "Firebase upload skipped: " + String(status.lastError));
        firebaseRegisterFailure(s_signInFailureCode, generation);
        return;
    }
    // Save/disable may have happened during the sign-in request.
    uint32_t latestGeneration;
    const auto latestSettings = firebaseReadConfig(&latestGeneration);
    if (latestGeneration != generation || !latestSettings.firebase_enabled) return;
    const auto snapshot = telemetryRead();
    if (!snapshot.sampleSequence) return; // startup has not produced a real cycle

    const String documentPath = firestoreDocumentPath(settings, s_fbDeviceUid);
    CloudName currentName;
    bool initializeName = false;
    if (telemetryUptimeMs() >= s_nextNameCheckMs)
    {
        currentName = firebaseReadCloudName(documentPath);
        if (currentName.status == 404 || (currentName.status == 200 && !currentName.name.length() && currentName.updateTime.length()))
            initializeName = true;
        else if (currentName.status == 200 && currentName.name.length())
        {
            rememberCloudName(currentName.name.c_str(), generation);
            s_nextNameCheckMs = telemetryUptimeMs() + 60000;
        }
        else
        {
            firebaseUpdateStatus(false, "Could not read the Firebase device name.");
            if (currentName.status == 401) s_fbTokenExpiryMs = 0;
            firebaseRegisterFailure(currentName.status, generation);
            return;
        }
    }
    const String url = "https://firestore.googleapis.com/v1/projects/" + String(settings.fb_project) +
        "/databases/(default)/documents:commit?key=" + String(settings.fb_api_key);
    WiFiClientSecure client;
    client.setCACert(FIREBASE_ROOT_CA);
    client.setHandshakeTimeout(5);
    HTTPClient https;
    https.setConnectTimeout(5000);
    https.setTimeout(5000);
    if (!https.begin(client, url))
    {
        firebaseUpdateStatus(false, "commit: HTTPClient begin() failed");
        firebaseRegisterFailure(-1, generation);
        return;
    }
    https.addHeader("Content-Type", "application/json");
    https.addHeader("Authorization", "Bearer " + s_fbIdToken);

    JsonDocument doc;
    JsonArray writes = doc["writes"].to<JsonArray>();
    JsonObject write = writes.add<JsonObject>();
    const bool built = writeTelemetryFirestore(write, snapshot, telemetryUptimeMs(), documentPath.c_str());
    if (built && initializeName)
    {
        write["update"]["fields"]["name"]["stringValue"] = snapshot.deviceId;
        write["updateMask"]["fieldPaths"].as<JsonArray>().add("name");
        if (currentName.status == 404) write["currentDocument"]["exists"] = false;
        else write["currentDocument"]["updateTime"] = currentName.updateTime;
    }
    if (!built || doc.overflowed())
    {
        https.end();
        firebaseUpdateStatus(false, "Could not build Firestore payload.");
        firebaseRegisterFailure(-1, generation);
        return;
    }
    String payload;
    serializeJson(doc, payload);
    const int code = https.POST(payload);
    if (code >= 200 && code < 300)
    {
        if (initializeName) s_nextNameCheckMs = telemetryUptimeMs() + 60000;
        JsonDocument receipt;
        if (!deserializeJson(receipt, https.getString()))
        {
            const char *stamp = receipt["writeResults"][0]["transformResults"][0]["timestampValue"] | "";
            portENTER_CRITICAL(&s_settingsMux);
            if (generation == s_settingsGeneration) strlcpy(s_cloudTimestamp, stamp, sizeof(s_cloudTimestamp));
            portEXIT_CRITICAL(&s_settingsMux);
        }
        firebaseUpdateStatus(true);
        s_fbConsecutiveFailures = 0;
        s_retryDelayMs = 0;
        s_nextAttemptMs = 0;
    }
    else
    {
        const String error = "Firestore commit HTTP " + String(code);
        firebaseUpdateStatus(false, error.c_str());
        if (code == 401) s_fbTokenExpiryMs = 0; // force a fresh token next time
        webLog(0, LOG_ERR, "Firebase upload failed: " + error);
        // A simultaneous first upload or app rename can win the precondition.
        firebaseRegisterFailure(initializeName && (code == 400 || code == 409 || code == 412) ? 0 : code, generation);
    }
    https.end();
}

static String pairingPageToken(const char *token)
{
    String encoded;
    for (const unsigned char *c = reinterpret_cast<const unsigned char *>(token); *c; ++c)
    {
        char escape[4];
        snprintf(escape, sizeof(escape), "%%%02X", static_cast<unsigned>(*c));
        encoded += escape;
    }
    return encoded;
}

static bool firebasePair(const PairRequest &request, JsonObject result, String &error)
{
    uint32_t generation;
    const auto settings = firebaseReadConfig(&generation);
    if (settings.fb_collection[0] && strcmp(settings.fb_collection, "devices") != 0)
    {
        error = "Set the Firestore collection to devices before pairing.";
        return false;
    }
    firebaseSyncWorkerGeneration(generation);
    if (WiFi.status() != WL_CONNECTED || !settings.fb_project[0] || !settings.fb_api_key[0] ||
        !firebaseEnsureIdToken(settings))
    {
        error = "Connect Wi-Fi and verify the saved Firebase credentials.";
        return false;
    }

    uint32_t latestGeneration;
    firebaseReadConfig(&latestGeneration);
    if (generation != latestGeneration)
    {
        error = "Device settings changed. Try again.";
        return false;
    }

    if (!s_fbDeviceUid.length())
    {
        error = "The Firebase upload account did not return a UID.";
        return false;
    }

    const String root = "projects/" + String(settings.fb_project) + "/databases/(default)/documents";
    const String devicePath = firestoreDocumentPath(settings, s_fbDeviceUid);
    const String base = "https://firestore.googleapis.com/v1/";
    String url;
    JsonDocument body;
    if (request.action == PairAction::Owners)
    {
        url = base + devicePath + "/owners?pageSize=20";
        if (request.argument[0]) url += "&pageToken=" + pairingPageToken(request.argument);
    }
    else if (request.action == PairAction::ReadName)
        url = base + devicePath + "?mask.fieldPaths=name";
    else
    {
        url = base + root + ":commit";
        JsonObject write = body["writes"].to<JsonArray>().add<JsonObject>();
        JsonArray mask = write["updateMask"]["fieldPaths"].to<JsonArray>();
        if (request.action == PairAction::Show)
        {
            if (!s_pairingHash[0] && !state_pairing_identity(s_pairingCode, s_pairingHash, true))
            {
                error = "Could not save the device pairing code.";
                return false;
            }
            write["update"]["name"] = root + "/pairingDevices/" + s_fbDeviceUid;
            write["update"]["fields"]["codeHash"]["stringValue"] = s_pairingHash;
            mask.add("codeHash");
        }
        else if (request.action == PairAction::Rename)
        {
            write["update"]["name"] = devicePath;
            write["update"]["fields"]["name"]["stringValue"] = String(request.argument);
            mask.add("name");
            write["currentDocument"]["exists"] = true;
        }
        else
        {
            write["update"]["name"] = devicePath + "/owners/" + request.argument;
            write["update"]["fields"]["blocked"]["booleanValue"] = request.action == PairAction::Block;
            mask.add("blocked");
            write["currentDocument"]["exists"] = true;
        }
    }
    if (body.overflowed())
    {
        error = "Could not prepare the pairing request.";
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(FIREBASE_ROOT_CA);
    client.setHandshakeTimeout(5);
    HTTPClient https;
    https.setConnectTimeout(5000);
    https.setTimeout(5000);
    if (!https.begin(client, url))
    {
        error = "Could not reach Firestore.";
        return false;
    }
    https.addHeader("Authorization", "Bearer " + s_fbIdToken);
    int status;
    if (request.action != PairAction::Owners && request.action != PairAction::ReadName)
    {
        https.addHeader("Content-Type", "application/json");
        String payload;
        serializeJson(body, payload);
        status = https.POST(payload);
    }
    else status = https.GET();

    if (status < 200 || status >= 300)
    {
        if (status == 401) s_fbTokenExpiryMs = 0;
        https.end();
        error = "Pairing request failed (HTTP " + String(status) + ").";
        return false;
    }

    result["type"] = "command_result";
    result["command"] = "pairing";
    result["ok"] = true;
    result["deviceUid"] = s_fbDeviceUid;
    if (request.action == PairAction::Show) result["code"] = String(s_pairingCode);
    else if (request.action == PairAction::Rename)
    {
        rememberCloudName(request.argument, generation);
        result["name"] = String(request.argument);
    }
    else if (request.action == PairAction::ReadName)
    {
        JsonDocument response;
        if (deserializeJson(response, https.getString()))
        {
            https.end();
            error = "Could not read the device name.";
            return false;
        }
        const char *name = response["fields"]["name"]["stringValue"] | "";
        if (firebaseValidDeviceName(name)) rememberCloudName(name, generation);
        result["name"] = String(name);
    }
    else if (request.action == PairAction::Owners)
    {
        JsonDocument response;
        const auto parsing = deserializeJson(response, https.getString());
        if (parsing)
        {
            error = "Could not read the linked accounts.";
            https.end();
            return false;
        }
        JsonArray owners = result["owners"].to<JsonArray>();
        for (JsonObjectConst owner : response["documents"].as<JsonArrayConst>())
        {
            const char *path = owner["name"] | "";
            const char *slash = strrchr(path, '/');
            if (!slash || !safePathId(slash + 1, 128)) continue;
            JsonObject entry = owners.add<JsonObject>();
            entry["uid"] = String(slash + 1);
            entry["label"] = String(owner["fields"]["label"]["stringValue"] | "");
            entry["blocked"] = owner["fields"]["blocked"]["booleanValue"] | false;
        }
        result["nextPageToken"] = String(response["nextPageToken"] | "");
    }
    https.end();
    return true;
}

static void firebaseWorker(void *)
{
    for (;;)
    {
        // pdTRUE clears all pending notifications: a bounded latest-state
        // request, rather than replaying each missed cadence/history item.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        portENTER_CRITICAL(&s_settingsMux);
        s_cloudBusy = true;
        const uint32_t testClientId = s_testClientId;
        s_testClientId = 0;
        const PairRequest pairing = s_pairRequest;
        s_pairRequest.clientId = 0;
        const uint32_t generation = s_settingsGeneration;
        portEXIT_CRITICAL(&s_settingsMux);
        if (testClientId)
        {
            String error;
            bool ok = firebaseTestConnection(error);
            portENTER_CRITICAL(&s_settingsMux);
            const bool changed = generation != s_settingsGeneration;
            portEXIT_CRITICAL(&s_settingsMux);
            if (changed)
            {
                ok = false;
                error = "Cloud settings changed during the test. Test again after saving.";
            }
            portENTER_CRITICAL(&s_settingsMux);
            s_resultOk = ok;
            strlcpy(s_resultError, error.c_str(), sizeof(s_resultError));
            s_resultClientId = testClientId;
            portEXIT_CRITICAL(&s_settingsMux);
        }
        else if (pairing.clientId)
        {
            JsonDocument result;
            String error;
            bool ok = firebasePair(pairing, result.to<JsonObject>(), error);
            uint32_t latest;
            firebaseReadConfig(&latest);
            if (generation != latest || result.overflowed())
            {
                ok = false;
                error = "Pairing settings changed or the response was too large. Try again.";
            }
            auto *reply = new (std::nothrow) PairReply{pairing.clientId, ok, error};
            if (reply && ok)
            {
                reply->payload = "";
                serializeJson(result, reply->payload);
            }
            portENTER_CRITICAL(&s_settingsMux);
            s_pairReply = reply;
            portEXIT_CRITICAL(&s_settingsMux);
        }
        else firebaseUploadCycle();
        portENTER_CRITICAL(&s_settingsMux);
        s_cloudBusy = false;
        portEXIT_CRITICAL(&s_settingsMux);
    }
}

void firebaseStartWorker()
{
    if (s_uploadWorker) return;
    if (xTaskCreatePinnedToCore(firebaseWorker, "FirebaseUpload", 12288, nullptr,
                               1, &s_uploadWorker, 0) != pdPASS)
    {
        s_uploadWorker = nullptr;
        webLog(0, LOG_ERR, "Firebase worker could not start. Local telemetry remains available.");
    }
}

void firebaseRequestUpload()
{
    portENTER_CRITICAL(&s_settingsMux);
    const bool ready = !s_cloudBusy && !s_testClientId && !s_pairRequest.clientId;
    portEXIT_CRITICAL(&s_settingsMux);
    if (ready && s_uploadWorker && currentConfig.firebase_enabled && WiFi.status() == WL_CONNECTED)
        xTaskNotifyGive(s_uploadWorker);
}

bool firebaseRequestTest(uint32_t clientId)
{
    if (!s_uploadWorker || !clientId) return false;
    portENTER_CRITICAL(&s_settingsMux);
    const bool available = !s_cloudBusy && !s_testClientId && !s_resultClientId && !s_pairRequest.clientId;
    if (available) s_testClientId = clientId;
    portEXIT_CRITICAL(&s_settingsMux);
    if (available) xTaskNotifyGive(s_uploadWorker);
    return available;
}

bool firebaseRequestPairing(uint32_t clientId, const char *action, const char *argument)
{
    if (!s_uploadWorker || !clientId || strlen(argument) >= sizeof(s_pairRequest.argument)) return false;
    PairRequest request{};
    request.clientId = clientId;
    if (strcmp(action, "show") == 0) request.action = PairAction::Show;
    else if (strcmp(action, "owners") == 0) request.action = PairAction::Owners;
    else if (strcmp(action, "block") == 0 && safePathId(argument, 128)) request.action = PairAction::Block;
    else if (strcmp(action, "unblock") == 0 && safePathId(argument, 128)) request.action = PairAction::Unblock;
    else if (strcmp(action, "read_name") == 0) request.action = PairAction::ReadName;
    else if (strcmp(action, "rename") == 0 && firebaseValidDeviceName(argument)) request.action = PairAction::Rename;
    else return false;
    strlcpy(request.argument, argument, sizeof(request.argument));

    portENTER_CRITICAL(&s_settingsMux);
    const bool available = !s_cloudBusy && !s_testClientId && !s_pairRequest.clientId && !s_pairReply;
    if (available) s_pairRequest = request;
    portEXIT_CRITICAL(&s_settingsMux);
    if (available) xTaskNotifyGive(s_uploadWorker);
    return available;
}

void firebaseNetworkLoop()
{
    portENTER_CRITICAL(&s_settingsMux);
    const bool persistDisable = s_pendingDisableGeneration &&
        s_pendingDisableGeneration == s_settingsGeneration && !currentConfig.firebase_enabled;
    s_pendingDisableGeneration = 0;
    char cloudName[sizeof(s_liveCloudName)]{};
    if (s_cloudNameGeneration == s_settingsGeneration) memcpy(cloudName, s_liveCloudName, sizeof(cloudName));
    s_liveCloudName[0] = '\0';
    portEXIT_CRITICAL(&s_settingsMux);
    if (cloudName[0] && strcmp(cloudName, currentConfig.device_id) != 0 && state_save_device_id(cloudName))
    {
        telemetrySetDeviceId(cloudName);
        broadcastConfig();
    }
    if (persistDisable)
    {
        state_save();
        broadcastConfig();
    }
    uint32_t clientId;
    bool ok;
    char error[sizeof(s_resultError)];
    portENTER_CRITICAL(&s_settingsMux);
    clientId = s_resultClientId;
    ok = s_resultOk;
    memcpy(error, s_resultError, sizeof(error));
    s_resultClientId = 0;
    PairReply *pairing = s_pairReply;
    s_pairReply = nullptr;
    portEXIT_CRITICAL(&s_settingsMux);
    if (pairing)
    {
        auto *client = ws.client(pairing->clientId);
        if (client && client->status() == WS_CONNECTED && wsClientIsAuthed(pairing->clientId))
        {
            if (pairing->ok) client->text(pairing->payload);
            else sendCmdAck(client, "pairing", false, pairing->payload);
        }
        delete pairing;
    }
    if (!clientId) return;
    webLog(0, ok ? LOG_INFO : LOG_ERR, ok ? "Firebase connection test succeeded." : "Firebase connection test failed: " + String(error));
    // Resolve the client again after the request; never retain a WS pointer
    // while waiting for HTTPS or send a result to a logged-out connection.
    auto *client = ws.client(clientId);
    if (client && client->status() == WS_CONNECTED && wsClientIsAuthed(clientId))
        sendCmdAck(client, "test_firebase", ok, error);
}
