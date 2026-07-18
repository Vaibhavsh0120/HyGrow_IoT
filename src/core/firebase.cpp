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
#include "state.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

static String s_fbIdToken;
static uint32_t s_fbTokenExpiryMs = 0; // millis() timestamp after which the cached token is considered stale

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
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(5000);

    String collection = String(currentConfig.fb_collection).length() > 0 ? String(currentConfig.fb_collection) : "sensor_data";
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
    String maskParams = "&updateMask.fieldPaths=uptime_s&updateMask.fieldPaths=server_timestamp";
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

    // Always-present bookkeeping fields -- not tied to any sensor.
    fields["uptime_s"]["integerValue"] = String(millis() / 1000);
    fields["server_timestamp"]["timestampValue"] = "REQUEST_TIME"; // Firestore special sentinel isn't supported via plain PATCH body; kept as a marker field for downstream tooling.

    String payload;
    serializeJson(doc, payload);

    int code = https.PATCH(payload);

    if (code >= 200 && code < 300)
    {
        currentVitals.firebase_ready = true;
        currentVitals.firebase_last_ok_ms = millis();
        currentVitals.firebase_last_error[0] = '\0';
    }
    else
    {
        currentVitals.firebase_ready = false;
        String err = "Firestore PATCH HTTP " + String(code);
        strncpy(currentVitals.firebase_last_error, err.c_str(), sizeof(currentVitals.firebase_last_error) - 1);
        webLog(0, LOG_ERR, "Firebase upload failed: " + err);
    }

    https.end();
}
