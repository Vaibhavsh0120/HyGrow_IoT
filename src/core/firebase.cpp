// ----------------------------------------------------------------------------
// firebase.cpp — Firebase / Firestore device-state upload (Part 5.3 / Part 6).
// ----------------------------------------------------------------------------
// Split out of the original task_network.cpp (see task_network_internal.h
// for the full map of the split).
//
// Minimal, non-blocking-per-call REST client that keeps ONE Firestore
// document per device — devices/{device_id} — in sync with the device's
// CURRENT state. "Non-blocking" here means: it never runs more often than
// currentConfig.interval_fb_ms, each HTTPClient call uses a short timeout, and
// it never retries in a loop — a slow/failed request just waits for the next
// cadence tick instead of stalling the network task. It does NOT run on a
// separate thread; a single request can still take up to ~timeout ms of wall
// time inside networkTaskLoop(), which is an accepted tradeoff for staying
// within the existing single-loop task structure and library set already in
// platformio.ini (no separate async-HTTP dependency).
//
// ---------------------------------------------------------------------------
// Device-state document contract (see docs/FIRESTORE_ARCHITECTURE.md for the
// full design writeup — this comment is the short version for anyone editing
// this file):
//
//   devices/{device_id}
//     deviceId       string    — mirrors currentConfig.device_id
//     status         string    — "Online", set by every successful upload
//                                 from THIS device. Never set to "Offline"
//                                 by the ESP32 — see point 2 below.
//     lastUpdated    timestamp — Firestore SERVER timestamp (fieldTransforms,
//                                 not a device-clock value), refreshed on
//                                 every successful upload.
//     uptime_s       integer   — device's own millis()/1000, informational.
//     firmwareVersion string   — compile-time constant, see FIRMWARE_VERSION.
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
//      the device Online. Offline is exclusively a BACKEND-derived state —
//      see functions/index.js's checkDeviceHeartbeats — computed from how
//      stale lastUpdated has become, not something this firmware ever
//      writes. That split (connectivity vs. sensor health) is deliberate:
//      see README.md's "Online vs. sensor health" note and
//      docs/FIRESTORE_ARCHITECTURE.md section 4.
// ---------------------------------------------------------------------------
#include "task_network_internal.h"
#include "task_network.h"
#include "state.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// Bumped by hand when firmware behavior meaningfully changes. Purely
// informational for the Firestore document / downstream apps — nothing in
// this firmware reads it back. Kept here (not config.h) since it is not a
// runtime-configurable value and has no NVS-backed override.
#define FIRMWARE_VERSION "1.1.0"

static String s_fbIdToken;
static uint32_t s_fbTokenExpiryMs = 0; // millis() timestamp after which the cached token is considered stale

// ----------------------------------------------------------------------------
// Auto-disable on repeated upload failure
// ----------------------------------------------------------------------------
// If Firestore uploads fail FIREBASE_MAX_CONSECUTIVE_FAILURES times in a row
// (sign-in failures and commit failures both count), Firebase Upload is
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
    String collection = String(currentConfig.fb_collection).length() > 0 ? String(currentConfig.fb_collection) : "devices";
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

// Counts one failed upload attempt (sign-in failure OR commit failure both
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

// Fires one Firestore commit with the current device-state snapshot. Called
// at most once per currentConfig.interval_fb_ms from networkTaskLoop()
// (task_network.cpp). Every call writes the FULL set of 8 telemetry fields
// plus deviceId/status/uptime_s/firmwareVersion — a field for a disabled or
// currently-failed sensor is written as an explicit Firestore null rather
// than omitted, so the document always reflects current sensor availability
// (see the file-level comment above for the full contract).
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

    // "devices" is the new default collection (one document per physical
    // device, keyed by device_id — see docs/FIRESTORE_ARCHITECTURE.md). Still
    // fully driven by currentConfig.fb_collection, same as before, so an
    // existing deployment that has already renamed its collection in
    // Settings keeps working without any firmware-side hardcoding.
    String collection = String(currentConfig.fb_collection).length() > 0 ? String(currentConfig.fb_collection) : "devices";
    String docId = String(currentConfig.device_id).length() > 0 ? String(currentConfig.device_id) : "esp32_device";
    String docPath = "projects/" + String(currentConfig.fb_project) +
                      "/databases/(default)/documents/" + collection + "/" + docId;

    // ------------------------------------------------------------------
    // Per-sensor availability -> null vs. real value.
    //
    // A sensor's field is REAL (doubleValue) only when it is enabled AND
    // its most recent read this boot succeeded (last_err[i] empty AND at
    // least one successful read has happened, i.e. last_ok_ms[i] != 0).
    // Every other case — disabled, never yet read, or currently erroring —
    // sends an explicit null. This is what actually clears a stale value
    // out of Firestore the moment a sensor stops being trustworthy, instead
    // of just no longer refreshing it.
    //
    // vpd_kpa is derived from DHT22 (computeVPD() in task_sensor.cpp) and
    // has no reading of its own, so it follows S_DHT's availability exactly
    // — if DHT22 is unavailable, vpd_kpa can't have been (re)computed this
    // cycle either, so it goes null right alongside temp_c/humidity.
    //
    // ALL 8 fields are listed in updateMask.fieldPaths on every request,
    // whether the value is a real number or null — that's what makes this a
    // full, atomic "current state" write instead of a partial patch: the
    // update mask controls which fields Firestore touches, and every field
    // in this document is meant to be touched every cycle. (Compare to the
    // previous version, which left a disabled sensor's field out of the
    // mask entirely — that meant Firestore silently kept whatever value was
    // written the last time the sensor was enabled, forever.)
    // Deliberately does NOT check sensorPinIsDemo() — a sensor currently
    // simulating data (demo_mode, or the per-sensor save_sensor_demo
    // toggle) still counts as "available" here and uploads exactly like a
    // real reading, with no distinguishing flag anywhere in the document.
    // Confirmed intentional: demo readings are meant to exercise the full
    // pipeline including the real Firestore write path, not just the
    // dashboard. If that ever needs to change, sensorPinIsDemo(id) (see
    // task_sensor.cpp) is the per-sensor check to add here.
    auto sensorAvailable = [](SensorID id) -> bool
    {
        return currentConfig.sensor_enabled[id] &&
               currentSensors.last_ok_ms[id] != 0 &&
               currentSensors.last_err[id][0] == '\0';
    };

    bool haveTds = sensorAvailable(S_TDS);
    bool haveDht = sensorAvailable(S_DHT); // also gates vpd_kpa
    bool haveWtemp = sensorAvailable(S_WTEMP);
    bool haveLight = sensorAvailable(S_LIGHT);
    bool havePh = sensorAvailable(S_PH);
    bool haveWl = sensorAvailable(S_WL);

    // Uses documents:commit (POST), NOT documents.patch (PATCH). This is a
    // deliberate choice, not a style preference: the plain PATCH endpoint's
    // body is only {"fields": {...}} — it has no field-transform mechanism
    // at all, so a "REQUEST_TIME" server timestamp is NOT achievable through
    // it (a previous version of this file tried sending a timestampValue
    // string through PATCH, which Firestore either rejects or stores as a
    // useless literal — see the removed NOTE this replaced). A genuine
    // server timestamp is only available via Write.updateTransforms, and
    // Write is a shape that only the :commit endpoint accepts. commit with
    // a single Write entry (update + updateMask + updateTransforms) is the
    // documented way to apply an ordinary field update and a server-value
    // transform to the same document atomically in one request — see
    // docs/FIRESTORE_ARCHITECTURE.md section 3 for the full citation trail.
    String url = "https://firestore.googleapis.com/v1/projects/" + String(currentConfig.fb_project) +
                 "/databases/(default)/documents:commit?key=" + String(currentConfig.fb_api_key);

    if (!https.begin(client, url))
    {
        currentVitals.firebase_ready = false;
        strncpy(currentVitals.firebase_last_error, "commit: HTTPClient begin() failed", sizeof(currentVitals.firebase_last_error) - 1);
        return;
    }
    https.addHeader("Content-Type", "application/json");
    https.addHeader("Authorization", "Bearer " + s_fbIdToken);

    // Firestore REST documents use a typed-value wrapper for every field.
    // A field set to {"nullValue": null} is a REAL, explicit null in
    // Firestore (distinct from the field not existing at all) — exactly
    // what "sensor unavailable" should mean downstream.
    JsonDocument doc;
    JsonArray writes = doc["writes"].to<JsonArray>();
    JsonObject write = writes.add<JsonObject>();
    JsonArray maskPaths = write["updateMask"]["fieldPaths"].to<JsonArray>();
    maskPaths.add("deviceId");
    maskPaths.add("status");
    maskPaths.add("uptime_s");
    maskPaths.add("firmwareVersion");
    maskPaths.add("tds_ppm");
    maskPaths.add("temp_c");
    maskPaths.add("humidity");
    maskPaths.add("vpd_kpa");
    maskPaths.add("water_temp_c");
    maskPaths.add("lux");
    maskPaths.add("ph_val");
    maskPaths.add("wl_percent");

    // lastUpdated is deliberately NOT in updateMask.fieldPaths above and NOT
    // in fields{} below — it is set exclusively via updateTransforms, the
    // only mechanism that produces a genuine Firestore SERVER timestamp
    // (setToServerValue: REQUEST_TIME). A plain fields["lastUpdated"] value
    // here would use whatever the ESP32 thinks the time is, which point 3
    // of the architecture explicitly forbids relying on for Offline
    // detection. Firestore applies updateTransforms AFTER update in the
    // same Write, so this and the fields{} below land atomically together.
    JsonObject transform = write["updateTransforms"].to<JsonArray>().add<JsonObject>();
    transform["fieldPath"] = "lastUpdated";
    transform["setToServerValue"] = "REQUEST_TIME";

    write["update"]["name"] = docPath;
    JsonObject fields = write["update"]["fields"].to<JsonObject>();

    fields["deviceId"]["stringValue"] = docId;
    fields["status"]["stringValue"] = "Online"; // connectivity, not sensor health — see file header
    fields["firmwareVersion"]["stringValue"] = FIRMWARE_VERSION;
    fields["uptime_s"]["integerValue"] = String(millis() / 1000);

    if (haveTds)
        fields["tds_ppm"]["doubleValue"] = currentSensors.tds_ppm;
    else
        fields["tds_ppm"]["nullValue"] = nullptr;

    if (haveDht)
    {
        fields["temp_c"]["doubleValue"] = currentSensors.temp_c;
        fields["humidity"]["doubleValue"] = currentSensors.humidity;
        fields["vpd_kpa"]["doubleValue"] = currentSensors.vpd_kpa;
    }
    else
    {
        fields["temp_c"]["nullValue"] = nullptr;
        fields["humidity"]["nullValue"] = nullptr;
        fields["vpd_kpa"]["nullValue"] = nullptr;
    }

    if (haveWtemp)
        fields["water_temp_c"]["doubleValue"] = currentSensors.water_temp_c;
    else
        fields["water_temp_c"]["nullValue"] = nullptr;

    if (haveLight)
        fields["lux"]["doubleValue"] = currentSensors.lux;
    else
        fields["lux"]["nullValue"] = nullptr;

    if (havePh)
        fields["ph_val"]["doubleValue"] = currentSensors.ph_val;
    else
        fields["ph_val"]["nullValue"] = nullptr;

    if (haveWl)
        fields["wl_percent"]["doubleValue"] = currentSensors.wl_percent;
    else
        fields["wl_percent"]["nullValue"] = nullptr;

    String payload;
    serializeJson(doc, payload);

    int code = https.POST(payload);

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
        String err = "Firestore commit HTTP " + String(code);
        strncpy(currentVitals.firebase_last_error, err.c_str(), sizeof(currentVitals.firebase_last_error) - 1);
        webLog(0, LOG_ERR, "Firebase upload failed: " + err);
        firebaseRegisterFailure();
    }

    https.end();
}
