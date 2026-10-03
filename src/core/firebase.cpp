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
// Device-state document contract:
//
//   devices/{device_id}
//     deviceId       string    — mirrors currentConfig.device_id
//     status         string    — "Online", set by every successful upload
//                                 from THIS device. Never set to "Offline"
//                                 by the ESP32 — see point 2 below.
//     lastUpdated    timestamp — Firestore SERVER timestamp (fieldTransforms,
//                                 not a device-clock value), refreshed on
//                                 every successful upload.
//     uptime_s       integer   — measured seconds since this boot, informational.
//     firmwareVersion string   — compile-time HYGROW_FIRMWARE_VERSION.
//     <8 sensor fields>        — one per telemetry value below.
//
// Two rules this file exists to enforce:
//
//   1. EVERY enabled sensor's field is written on EVERY upload — either a
//      real doubleValue, or an explicit Firestore nullValue if that sensor
//      is disabled/unavailable/mid-failure. A field is only left out of the
//      update mask entirely when the sensor was disabled at compile-time-
//      never (S_COUNT is fixed at 6, all 8 telemetry fields always exist).
//      This is the fix for the old behavior, where a disabled sensor's
//      field was dropped from BOTH the body and the update mask — which
//      left Firestore holding that sensor's last real value forever, with
//      nothing downstream able to tell "still reading 6.2" apart from
//      "hasn't reported since the probe was unplugged three weeks ago".
//   2. status/lastUpdated always mean "this device (Wi-Fi + Firestore
//      reachability) is alive", never "every sensor is healthy". A single
//      failed sensor still uploads successfully (as null) and still marks
//      the device Online. This firmware NEVER writes "Offline" itself —
//      that has to be derived by whatever reads this collection (e.g. a
//      scheduled backend job that flags a device Offline once lastUpdated
//      goes stale). That split (connectivity vs. sensor health) is
//      deliberate — see README.md's Cloud Sync section.
// ---------------------------------------------------------------------------
#include "task_network_internal.h"
#include "task_network.h"
#include "state.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "telemetry.h"
#include "firebase_retry.h"

static String s_fbIdToken;
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
static uint32_t s_testClientId = 0;
static uint32_t s_resultClientId = 0;
static bool s_resultOk = false;
static char s_resultError[160]{};

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

// On-demand connectivity check for the Settings > Cloud Provisioning
// "Test Connection" button (test_firebase command, command_handlers.cpp).
// Performs a REAL sign-in against Identity Toolkit with whatever is
// currently saved in currentConfig (not a hand-typed value from the form —
// the button only makes sense after Save Credentials has run), and a real
// Firestore GET against the configured project/collection/device document so
// the reported result reflects genuine reachability, not just "the fields
// are non-empty". Deliberately does NOT touch/reuse firebaseUploadCycle()'s
// cached token (s_fbIdToken) — a stale cached token from *before* a
// credential change could report "ok" for credentials that no longer work,
// which would defeat the entire point of a manual test. errorOut is only
// written when this returns false.
bool firebaseTestConnection(String &errorOut)
{
    const auto settings = firebaseReadConfig();
    if (String(settings.fb_api_key).length() == 0 ||
        String(settings.fb_email).length() == 0 ||
        String(settings.fb_pass).length() == 0)
    {
        errorOut = "Missing Web API Key, Email, or Password.";
        return false;
    }
    if (String(settings.fb_project).length() == 0)
    {
        errorOut = "Missing Project ID.";
        return false;
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        errorOut = "Device is not connected to Wi-Fi.";
        return false;
    }

    // 1. Sign in — proves the API key + email/password are valid together.
    WiFiClientSecure signInClient;
    signInClient.setInsecure();
    signInClient.setHandshakeTimeout(2);
    HTTPClient signInHttps;
    signInHttps.setConnectTimeout(2000);
    signInHttps.setTimeout(3000);

    String signInUrl = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + String(settings.fb_api_key);
    if (!signInHttps.begin(signInClient, signInUrl))
    {
        errorOut = "Could not start sign-in request.";
        return false;
    }
    signInHttps.addHeader("Content-Type", "application/json");

    JsonDocument signInBody;
    signInBody["email"] = settings.fb_email;
    signInBody["password"] = settings.fb_pass;
    signInBody["returnSecureToken"] = true;
    String signInBodyStr;
    serializeJson(signInBody, signInBodyStr);

    int signInCode = signInHttps.POST(signInBodyStr);
    String testToken;

    if (signInCode == 200)
    {
        JsonDocument resp;
        DeserializationError err = deserializeJson(resp, signInHttps.getString());
        if (!err && resp["idToken"].is<const char *>())
        {
            testToken = String((const char *)resp["idToken"]);
        }
        else
        {
            signInHttps.end();
            errorOut = "Sign-in succeeded but returned a malformed response.";
            return false;
        }
    }
    else
    {
        String body = signInHttps.getString();
        signInHttps.end();
        // Identity Toolkit's error payload has {"error":{"message":"..."}}
        // with short, stable machine-readable codes — surface that directly
        // instead of just the HTTP status, e.g. "INVALID_PASSWORD" /
        // "EMAIL_NOT_FOUND" / "API key not valid" are far more actionable
        // than "HTTP 400".
        JsonDocument errDoc;
        String reason = "HTTP " + String(signInCode);
        if (!deserializeJson(errDoc, body) && errDoc["error"]["message"].is<const char *>())
        {
            reason = String((const char *)errDoc["error"]["message"]);
        }
        errorOut = "Sign-in failed: " + reason;
        return false;
    }
    signInHttps.end();

    // 2. A lightweight authenticated Firestore GET — proves the Project ID
    // is real and this account can actually reach it, not just that the
    // Identity Toolkit login worked in isolation (a valid login against the
    // wrong project would otherwise report a false "ok").
    String collection = String(settings.fb_collection).length() > 0 ? String(settings.fb_collection) : "devices";
    const auto snapshot = telemetryRead();
    String docId = snapshot.deviceId;

    WiFiClientSecure fsClient;
    fsClient.setInsecure();
    fsClient.setHandshakeTimeout(2);
    HTTPClient fsHttps;
    fsHttps.setConnectTimeout(2000);
    fsHttps.setTimeout(3000);

    String fsUrl = "https://firestore.googleapis.com/v1/projects/" + String(settings.fb_project) +
                   "/databases/(default)/documents/" + collection + "/" + docId +
                   "?key=" + String(settings.fb_api_key);
    if (!fsHttps.begin(fsClient, fsUrl))
    {
        errorOut = "Signed in, but could not start the Firestore check.";
        return false;
    }
    fsHttps.addHeader("Authorization", "Bearer " + testToken);

    int fsCode = fsHttps.GET();
    String fsBody = fsHttps.getString();
    fsHttps.end();

    // A GET on a document that doesn't exist YET (204/404-shaped 200 with no
    // fields, or a genuine 404) is still a successful connection — it means
    // the project/credentials/permissions are all correct and the very next
    // upload cycle will simply create that document. Only treat this as a
    // failure for errors that mean the connection itself didn't work
    // (bad project id, permission denied, etc).
    if (fsCode == 200 || fsCode == 404)
    {
        return true;
    }

    JsonDocument errDoc;
    String reason = "HTTP " + String(fsCode);
    if (!deserializeJson(errDoc, fsBody) && errDoc["error"]["message"].is<const char *>())
    {
        reason = String((const char *)errDoc["error"]["message"]);
    }
    errorOut = "Signed in, but Firestore check failed: " + reason;
    return false;
}

// Exchange fb_email/fb_pass for a Firebase Identity Toolkit ID token.
// Caches the token and its expiry so normal upload cycles don't sign in
// every time — only when the cache is empty or has expired.
static bool firebaseEnsureIdToken(const FirebaseSettings &settings)
{
    s_signInFailureCode = 0;
    if (s_fbIdToken.length() > 0 && (int32_t)(millis() - s_fbTokenExpiryMs) < 0)
    {
        return true; // cached token still valid
    }

    if (String(settings.fb_api_key).length() == 0 ||
        String(settings.fb_email).length() == 0 ||
        String(settings.fb_pass).length() == 0)
    {
        s_signInFailureCode = 400;
        firebaseUpdateStatus(false, "Missing Firebase email/password/API key");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure(); // Google's public CA chain rotates; verifying isn't practical on-device with limited flash for a CA bundle here.
    client.setHandshakeTimeout(5);
    HTTPClient https;
    https.setConnectTimeout(5000);
    https.setTimeout(5000);

    String url = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + String(settings.fb_api_key);
    if (!https.begin(client, url))
    {
        firebaseUpdateStatus(false, "signIn: HTTPClient begin() failed");
        return false;
    }
    https.addHeader("Content-Type", "application/json");

    JsonDocument body;
    body["email"] = settings.fb_email;
    body["password"] = settings.fb_pass;
    body["returnSecureToken"] = true;
    String bodyStr;
    serializeJson(body, bodyStr);

    int code = https.POST(bodyStr);
    s_signInFailureCode = code;
    bool ok = false;

    if (code == 200)
    {
        JsonDocument resp;
        DeserializationError err = deserializeJson(resp, https.getString());
        if (!err && resp["idToken"].is<const char *>())
        {
            s_fbIdToken = String((const char *)resp["idToken"]);
            long expiresIn = resp["expiresIn"] | 3600; // seconds, Firebase default 1hr tokens
            // Refresh a little early (80% of lifetime) to avoid racing expiry mid-upload.
            s_fbTokenExpiryMs = millis() + (uint32_t)(expiresIn * 800UL);
            ok = true;
        }
        else
        {
            firebaseUpdateStatus(false, "signIn: malformed token response");
        }
    }
    else
    {
        String err = "signIn HTTP " + String(code);
        firebaseUpdateStatus(false, err.c_str());
    }

    https.end();
    return ok;
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
        }
    }
    portEXIT_CRITICAL(&s_settingsMux);
    if (disabled)
    {
        state_save();
        webLog(0, LOG_ERR, "Firebase has 5 permanent configuration/permission failures. Fix Cloud Provisioning, then re-enable uploads.");
        broadcastConfig();
    }
}

// Called exclusively by the cloud worker. Upload the newest complete sensor
// cycle after sign-in, with the exact same JSON fields as GET /status.
void firebaseUploadCycle()
{
    uint32_t generation;
    const auto settings = firebaseReadConfig(&generation);
    if (!settings.firebase_enabled || WiFi.status() != WL_CONNECTED ||
        !settings.fb_project[0] || !settings.fb_api_key[0]) return;

    if (generation != s_workerGeneration)
    {
        s_fbIdToken = "";
        s_fbTokenExpiryMs = 0;
        s_fbConsecutiveFailures = 0;
        s_retryDelayMs = 0;
        s_nextAttemptMs = 0;
        s_workerGeneration = generation;
    }
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

    const String collection = settings.fb_collection[0] ? settings.fb_collection : "devices";
    const String documentPath = "projects/" + String(settings.fb_project) +
        "/databases/(default)/documents/" + collection + "/" + snapshot.deviceId;
    const String url = "https://firestore.googleapis.com/v1/projects/" + String(settings.fb_project) +
        "/databases/(default)/documents:commit?key=" + String(settings.fb_api_key);
    WiFiClientSecure client;
    client.setInsecure(); // existing TLS policy; no change to trust configuration
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
    JsonObject write = doc["writes"].to<JsonArray>().add<JsonObject>();
    writeTelemetryFirestore(write, snapshot, telemetryUptimeMs(), documentPath.c_str());
    String payload;
    serializeJson(doc, payload);
    const int code = https.POST(payload);
    if (code >= 200 && code < 300)
    {
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
        firebaseRegisterFailure(code, generation);
    }
    https.end();
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
    const bool ready = !s_cloudBusy && !s_testClientId;
    portEXIT_CRITICAL(&s_settingsMux);
    if (ready && s_uploadWorker && currentConfig.firebase_enabled && WiFi.status() == WL_CONNECTED)
        xTaskNotifyGive(s_uploadWorker);
}

bool firebaseRequestTest(uint32_t clientId)
{
    if (!s_uploadWorker || !clientId) return false;
    portENTER_CRITICAL(&s_settingsMux);
    const bool available = !s_cloudBusy && !s_testClientId && !s_resultClientId;
    if (available) s_testClientId = clientId;
    portEXIT_CRITICAL(&s_settingsMux);
    if (available) xTaskNotifyGive(s_uploadWorker);
    return available;
}

void firebaseNetworkLoop()
{
    uint32_t clientId;
    bool ok;
    char error[sizeof(s_resultError)];
    portENTER_CRITICAL(&s_settingsMux);
    clientId = s_resultClientId;
    ok = s_resultOk;
    memcpy(error, s_resultError, sizeof(error));
    s_resultClientId = 0;
    portEXIT_CRITICAL(&s_settingsMux);
    if (!clientId) return;
    webLog(0, ok ? LOG_INFO : LOG_ERR, ok ? "Firebase connection test succeeded." : "Firebase connection test failed: " + String(error));
    // Resolve the client again after the request; never retain a WS pointer
    // while waiting for HTTPS or send a result to a logged-out connection.
    auto *client = ws.client(clientId);
    if (client && client->status() == WS_CONNECTED && wsClientIsAuthed(clientId))
        sendCmdAck(client, "test_firebase", ok, error);
}
