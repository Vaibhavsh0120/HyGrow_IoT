// ----------------------------------------------------------------------------
// firebase.cpp — Firebase / Firestore upload (Part 5.3).
// ----------------------------------------------------------------------------
// Split out of the original task_network.cpp (see task_network_internal.h
// for the full map of the split). No functional changes — this is a
// structural split only.
//
// Minimal, non-blocking-per-call REST client for pushing sensor readings to
// Firestore. "Non-blocking" here means: it never runs more often than
// currentConfig.interval_fb_ms, each HTTPClient call uses a short timeout, and
// it never retries in a loop — a slow/failed request just waits for the next
// cadence tick instead of stalling the network task. It does NOT run on a
// separate thread; a single request can still take up to ~timeout ms of wall
// time inside networkTaskLoop(), which is an accepted tradeoff for staying
// within the existing single-loop task structure and library set already in
// platformio.ini (no separate async-HTTP dependency).
// ----------------------------------------------------------------------------
#include "task_network_internal.h"
#include "task_network.h"
#include "state.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

static String s_fbIdToken;
static uint32_t s_fbTokenExpiryMs = 0; // millis() timestamp after which the cached token is considered stale

// ----------------------------------------------------------------------------
// Auto-disable on repeated upload failure
// ----------------------------------------------------------------------------
// If Firestore uploads fail FIREBASE_MAX_CONSECUTIVE_FAILURES times in a row
// (sign-in failures and PATCH failures both count), Firebase Upload is
// switched off automatically and persisted — the same single on/off switch
// (currentConfig.firebase_enabled) the "Firebase Upload" toggle in Settings
// controls, so the UI reflects this the moment it happens instead of the
// device silently retrying bad credentials/an unreachable project forever.
// The counter resets to 0 on any successful upload, and separately whenever
// save_firebase saves new credentials (command_handlers.cpp) or the user
// re-enables the toggle (save_features) — both are a fresh reason to try
// again from zero.
#define FIREBASE_MAX_CONSECUTIVE_FAILURES 5
static uint8_t s_fbConsecutiveFailures = 0;

// Forces the next firebaseUploadCycle() to sign in again from scratch
// instead of reusing a cached ID token. Must be called whenever
// fb_email/fb_pass/fb_project/fb_api_key change (see save_firebase in
// command_handlers.cpp) — without this, a credential change while a
// still-valid cached token exists would keep uploading under the OLD
// identity/project until that token's ~1hr lifetime naturally expired,
// silently ignoring the just-saved credentials in the meantime. Also resets
// the consecutive-failure counter — new credentials deserve a fresh set of
// attempts rather than immediately auto-disabling on the leftover count
// from the old (bad) ones.
void firebaseInvalidateToken()
{
    s_fbIdToken = "";
    s_fbTokenExpiryMs = 0;
    s_fbConsecutiveFailures = 0;
}

// Called from save_features (command_handlers.cpp) whenever the user
// switches Firebase Upload back ON — including right after an auto-disable.
// Manually re-enabling is an explicit "try again" signal, so the failure
// count starts over instead of auto-disabling again on the very next tick
// with 0 fresh attempts made.
void firebaseResetFailureCount()
{
    s_fbConsecutiveFailures = 0;
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
    if (String(currentConfig.fb_api_key).length() == 0 ||
        String(currentConfig.fb_email).length() == 0 ||
        String(currentConfig.fb_pass).length() == 0)
    {
        errorOut = "Missing Web API Key, Email, or Password.";
        return false;
    }
    if (String(currentConfig.fb_project).length() == 0)
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
    HTTPClient signInHttps;
    signInHttps.setTimeout(7000);

    String signInUrl = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + String(currentConfig.fb_api_key);
    if (!signInHttps.begin(signInClient, signInUrl))
    {
        errorOut = "Could not start sign-in request.";
        return false;
    }
    signInHttps.addHeader("Content-Type", "application/json");

    JsonDocument signInBody;
    signInBody["email"] = currentConfig.fb_email;
    signInBody["password"] = currentConfig.fb_pass;
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
    String collection = String(currentConfig.fb_collection).length() > 0 ? String(currentConfig.fb_collection) : "sensor_readings";
    String docId = String(currentConfig.device_id).length() > 0 ? String(currentConfig.device_id) : "esp32_device";

    WiFiClientSecure fsClient;
    fsClient.setInsecure();
    HTTPClient fsHttps;
    fsHttps.setTimeout(7000);

    String fsUrl = "https://firestore.googleapis.com/v1/projects/" + String(currentConfig.fb_project) +
                   "/databases/(default)/documents/" + collection + "/" + docId +
                   "?key=" + String(currentConfig.fb_api_key);
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
static bool firebaseEnsureIdToken()
{
    if (s_fbIdToken.length() > 0 && (int32_t)(millis() - s_fbTokenExpiryMs) < 0)
    {
        return true; // cached token still valid
    }

    if (String(currentConfig.fb_api_key).length() == 0 ||
        String(currentConfig.fb_email).length() == 0 ||
        String(currentConfig.fb_pass).length() == 0)
    {
        strncpy(currentVitals.firebase_last_error, "Missing Firebase email/password/API key", sizeof(currentVitals.firebase_last_error) - 1);
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure(); // Google's public CA chain rotates; verifying isn't practical on-device with limited flash for a CA bundle here.
    HTTPClient https;
    https.setTimeout(5000);

    String url = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + String(currentConfig.fb_api_key);
    if (!https.begin(client, url))
    {
        strncpy(currentVitals.firebase_last_error, "signIn: HTTPClient begin() failed", sizeof(currentVitals.firebase_last_error) - 1);
        return false;
    }
    https.addHeader("Content-Type", "application/json");

    JsonDocument body;
    body["email"] = currentConfig.fb_email;
    body["password"] = currentConfig.fb_pass;
    body["returnSecureToken"] = true;
    String bodyStr;
    serializeJson(body, bodyStr);

    int code = https.POST(bodyStr);
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
            strncpy(currentVitals.firebase_last_error, "signIn: malformed token response", sizeof(currentVitals.firebase_last_error) - 1);
        }
    }
    else
    {
        String err = "signIn HTTP " + String(code);
        strncpy(currentVitals.firebase_last_error, err.c_str(), sizeof(currentVitals.firebase_last_error) - 1);
    }

    https.end();
    return ok;
}

// Counts one failed upload attempt (sign-in failure OR PATCH failure both
// call this). Once FIREBASE_MAX_CONSECUTIVE_FAILURES is hit in a row,
// switches currentConfig.firebase_enabled off, persists it, and broadcasts
// the new config so the Settings > Firebase Upload toggle flips to OFF in
// every open browser tab immediately — the same live-reflects-device-state
// path save_features already uses (command_handlers.cpp), just triggered
// from here instead of a user click.
static void firebaseRegisterFailure()
{
    if (s_fbConsecutiveFailures < 255)
        s_fbConsecutiveFailures++;

    if (s_fbConsecutiveFailures >= FIREBASE_MAX_CONSECUTIVE_FAILURES && currentConfig.firebase_enabled)
    {
        currentConfig.firebase_enabled = false;
        state_save();
        webLog(0, LOG_ERR, "Firebase upload failed " + String(FIREBASE_MAX_CONSECUTIVE_FAILURES) +
                                " times in a row — Firebase Upload turned OFF automatically. "
                                "Fix the credentials/connection in Settings, Test Connection, then re-enable.");
        broadcastConfig();
    }
}

// Fires one Firestore PATCH with the current sensor snapshot. Called at most
// once per currentConfig.interval_fb_ms from networkTaskLoop() (task_network.cpp).
void firebaseUploadCycle()
{
    if (!currentConfig.firebase_enabled)
        return;
    if (WiFi.status() != WL_CONNECTED)
        return;
    if (String(currentConfig.fb_project).length() == 0 || String(currentConfig.fb_api_key).length() == 0)
        return;

    if (!firebaseEnsureIdToken())
    {
        currentVitals.firebase_ready = false;
        webLog(0, LOG_ERR, "Firebase upload skipped: " + String(currentVitals.firebase_last_error));
        firebaseRegisterFailure();
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(5000);

    String collection = String(currentConfig.fb_collection).length() > 0 ? String(currentConfig.fb_collection) : "sensor_readings";
    String docId = String(currentConfig.device_id).length() > 0 ? String(currentConfig.device_id) : "esp32_device";

    // ------------------------------------------------------------------
    // Per-sensor field gating (fixes a real bug): a field is only written
    // to the outgoing document -- and only listed in the PATCH updateMask --
    // if the sensor(s) it comes from are actually enabled. Previously every
    // field was written unconditionally, so a disabled sensor's last stale
    // in-memory value (or 0/NaN if it was never read this boot) kept
    // getting uploaded to Firestore forever, which is misleading downstream
    // (e.g. a chart that thinks pH is being monitored when the probe was
    // turned off weeks ago).
    //
    // vpd_kpa is a derived field (computeVPD() in task_sensor.cpp), computed
    // only from DHT22's temp_c/humidity -- it has no independent sensor of
    // its own. It's gated on S_DHT specifically for that reason: if DHT is
    // off, VPD can't be (re)computed and holds a stale/zero value, so it's
    // dropped from the payload along with temp_c/humidity. If more sensors
    // ever feed into VPD's calculation, extend this same condition to also
    // require those be enabled.
    //
    // updateMask.fieldPaths is a Firestore QUERY PARAMETER (not a body
    // field) -- see the REST docs for projects.databases.documents.patch.
    // Setting it means a field left out of both the mask and the body is
    // simply not touched on the existing document, rather than silently
    // frozen at whatever value the last successful upload wrote -- "sensor
    // off" should read as "no updates being made to that field", not
    // "value permanently stuck at its last live reading".
    bool wantTds = currentConfig.sensor_enabled[S_TDS];
    bool wantDht = currentConfig.sensor_enabled[S_DHT]; // also gates vpd_kpa
    bool wantWtemp = currentConfig.sensor_enabled[S_WTEMP];
    bool wantLight = currentConfig.sensor_enabled[S_LIGHT];
    bool wantPh = currentConfig.sensor_enabled[S_PH];
    bool wantWl = currentConfig.sensor_enabled[S_WL];

    // updateMask.fieldPaths must appear as one repeated query param per
    // field -- Firestore does not accept a single comma-joined param here.
    String maskParams = "&updateMask.fieldPaths=uptime_s";
    if (wantTds)
        maskParams += "&updateMask.fieldPaths=tds_ppm";
    if (wantDht)
        maskParams += "&updateMask.fieldPaths=temp_c&updateMask.fieldPaths=humidity&updateMask.fieldPaths=vpd_kpa";
    if (wantWtemp)
        maskParams += "&updateMask.fieldPaths=water_temp_c";
    if (wantLight)
        maskParams += "&updateMask.fieldPaths=lux";
    if (wantPh)
        maskParams += "&updateMask.fieldPaths=ph_val";
    if (wantWl)
        maskParams += "&updateMask.fieldPaths=wl_percent";

    String url = "https://firestore.googleapis.com/v1/projects/" + String(currentConfig.fb_project) +
                 "/databases/(default)/documents/" + collection + "/" + docId +
                 "?key=" + String(currentConfig.fb_api_key) + maskParams;

    if (!https.begin(client, url))
    {
        currentVitals.firebase_ready = false;
        strncpy(currentVitals.firebase_last_error, "PATCH: HTTPClient begin() failed", sizeof(currentVitals.firebase_last_error) - 1);
        return;
    }
    https.addHeader("Content-Type", "application/json");
    https.addHeader("Authorization", "Bearer " + s_fbIdToken);

    // Firestore REST documents use a typed-value wrapper for every field.
    JsonDocument doc;
    JsonObject fields = doc["fields"].to<JsonObject>();

    if (wantTds)
        fields["tds_ppm"]["doubleValue"] = currentSensors.tds_ppm;
    if (wantDht)
    {
        fields["temp_c"]["doubleValue"] = currentSensors.temp_c;
        fields["humidity"]["doubleValue"] = currentSensors.humidity;
        fields["vpd_kpa"]["doubleValue"] = currentSensors.vpd_kpa;
    }
    if (wantWtemp)
        fields["water_temp_c"]["doubleValue"] = currentSensors.water_temp_c;
    if (wantLight)
        fields["lux"]["doubleValue"] = currentSensors.lux;
    if (wantPh)
        fields["ph_val"]["doubleValue"] = currentSensors.ph_val;
    if (wantWl)
        fields["wl_percent"]["doubleValue"] = currentSensors.wl_percent;

    // Always-present bookkeeping field -- not tied to any sensor.
    //
    // NOTE: this used to also send fields["server_timestamp"]["timestampValue"]
    // = "REQUEST_TIME", intending Firestore's server-timestamp sentinel. That
    // sentinel is only honored via a field TRANSFORM
    // (fieldTransforms[].setToServerValue in the REST API), not as a plain
    // field value in a PATCH body -- sending it as a timestampValue string
    // either gets rejected as an invalid timestamp or stored as a literal,
    // useless string, on every single upload. Removed rather than fixed
    // properly with a transform, since uptime_s below (device clock, already
    // sent) plus currentVitals.firebase_last_ok_ms (millis() of the last
    // successful upload, tracked device-side below) already cover freshness
    // without needing a second, server-side timestamp field.
    fields["uptime_s"]["integerValue"] = String(millis() / 1000);

    String payload;
    serializeJson(doc, payload);

    int code = https.PATCH(payload);

    if (code >= 200 && code < 300)
    {
        currentVitals.firebase_ready = true;
        currentVitals.firebase_last_ok_ms = millis();
        currentVitals.firebase_last_error[0] = '\0';
        s_fbConsecutiveFailures = 0; // any successful upload clears the streak
    }
    else
    {
        currentVitals.firebase_ready = false;
        String err = "Firestore PATCH HTTP " + String(code);
        strncpy(currentVitals.firebase_last_error, err.c_str(), sizeof(currentVitals.firebase_last_error) - 1);
        webLog(0, LOG_ERR, "Firebase upload failed: " + err);
        firebaseRegisterFailure();
    }

    https.end();
}
