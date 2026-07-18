#include "task_network.h"
#include "state.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <set>

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

unsigned long lastVitalsTime = 0;
unsigned long lastDataTime = 0;
unsigned long lastFirebaseTime = 0;

// ----------------------------------------------------------------------------
// WebSocket auth gatekeeper (single-owner login)
// ----------------------------------------------------------------------------
// AsyncWebSocketClient has no free field to flag "authenticated", so
// authenticated client ids are tracked in their own set here instead. A
// client's id is unique for the lifetime of its connection (AsyncWebSocket
// assigns a fresh one per connect), and WS_EVT_DISCONNECT below removes it,
// so this set never grows unbounded and never confuses one browser tab's
// session with another's.
static std::set<uint32_t> s_authedClients;

static bool wsClientIsAuthed(uint32_t clientId)
{
    return s_authedClients.find(clientId) != s_authedClients.end();
}

// Sends `payload` only to clients that have completed the auth handshake —
// used by every broadcast*() function below instead of ws.textAll(), so an
// unauthenticated connection (pre-login, or one that never logs in at all)
// never receives live telemetry, config, or vitals data, regardless of
// whether it arrived over Wi-Fi STA or the SoftAP.
static void wsTextAllAuthed(const String &payload)
{
    for (AsyncWebSocketClient &c : ws.getClients())
    {
        if (c.status() == WS_CONNECTED && wsClientIsAuthed(c.id()))
        {
            c.text(payload);
        }
    }
}

// Public entry point declared in task_network.h — see the comment there for
// why this thin wrapper exists instead of exposing wsTextAllAuthed() itself.
void wsBroadcastLog(const String &payload)
{
    wsTextAllAuthed(payload);
}

// Sends the very first frame a client sees on connect: whether the device
// still needs its first password set ("setup_required": true) or already has
// one and is waiting for a login ("setup_required": false). The frontend
// (data/js/app.js) branches its overlay purely off this flag.
static void sendAuthStatus(AsyncWebSocketClient *client)
{
    JsonDocument doc;
    doc["type"] = "auth_status";
    doc["setup_required"] = !auth_is_configured();
    String payload;
    serializeJson(doc, payload);
    client->text(payload);
}

// Internal helpers
void handleWebSocketMessage(AsyncWebSocketClient *client, void *arg, uint8_t *data, size_t len);
static void handleAuthCommand(AsyncWebSocketClient *client, JsonDocument &doc);
static void handleChangePasswordCommand(AsyncWebSocketClient *client, JsonDocument &doc);
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void firebaseUploadCycle();

// ----------------------------------------------------------------------------
// Firebase / Firestore upload (Part 5.3)
// ----------------------------------------------------------------------------
// Minimal, non-blocking-per-call REST client for pushing sensor readings to
// Firestore. "Non-blocking" here means: it never runs more often than
// currentConfig.interval_fb_ms, each HTTPClient call uses a short timeout, and
// it never retries in a loop — a slow/failed request just waits for the next
// cadence tick instead of stalling the network task. It does NOT run on a
// separate thread; a single request can still take up to ~timeout ms of wall
// time inside networkTaskLoop(), which is an accepted tradeoff for staying
// within the existing single-loop task structure and library set already in
// platformio.ini (no separate async-HTTP dependency).
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
// once per currentConfig.interval_fb_ms from networkTaskLoop().
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

void initNetworkTask()
{
    webLog(0, LOG_INFO, "Initializing Network Task...");

    // 1. Wi-Fi Setup with SoftAP Fallback
    WiFi.mode(WIFI_STA);
    if (String(currentConfig.wifi_ssid).length() > 0)
    {
        WiFi.begin(currentConfig.wifi_ssid, currentConfig.wifi_pass);
        webLog(0, LOG_INFO, "Connecting to Wi-Fi: " + String(currentConfig.wifi_ssid));

        unsigned long startAttempt = millis();
        // 15-second timeout for STA
        while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000)
        {
            delay(500);
        }
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        webLog(0, LOG_WARN, "STA connection failed. Starting SoftAP fallback.");
        WiFi.mode(WIFI_AP_STA); // Keep STA active in background to allow dynamic reconnects if possible
        WiFi.softAP("HyGrow-Setup", currentConfig.ap_pass);
        webLog(0, LOG_INFO, "SoftAP IP: " + WiFi.softAPIP().toString());
    }
    else
    {
        webLog(0, LOG_INFO, "Wi-Fi Connected. IP: " + WiFi.localIP().toString());
    }

    // 2. Web Server & File System
    // NOTE: LittleFS is already mounted once in state_init() (see src/core/state.cpp),
    // and a bad mount there now halts boot before this task ever runs. Calling
    // LittleFS.begin() again here was redundant — it just re-returned true on an
    // already-mounted filesystem. We reuse the vitals flag set by state_init() instead.
    if (!currentVitals.littlefs_ok)
    {
        webLog(0, LOG_ERR, "LittleFS Mount Failed!");
    }
    else
    {
        server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    }

    // 3. Web Server & File System Setup Complete
    // (Native OTA Mount removed for attack surface reduction)

    // 4. WebSocket Mount
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // 5. Start Server
    server.begin();
    webLog(0, LOG_INFO, "Web Server started on port 80");
}

void networkTaskLoop()
{
    ws.cleanupClients();

    unsigned long currentMillis = millis();

    // Dynamic cadence for Vitals (was a hardcoded 1000ms literal — now backed
    // by currentConfig.interval_vitals_ms, same pattern as the other timers).
    if (currentMillis - lastVitalsTime >= currentConfig.interval_vitals_ms)
    {
        lastVitalsTime = currentMillis;
        broadcastVitals();
    }

    // Dynamic cadence for Data Push based on configuration
    if (currentMillis - lastDataTime >= currentConfig.interval_ws_ms)
    {
        lastDataTime = currentMillis;
        broadcastData();
    }

    // Dynamic cadence for the Firestore upload cycle. firebaseUploadCycle()
    // itself no-ops immediately if firebase_enabled/Wi-Fi/credentials aren't
    // ready, so it's safe to call unconditionally here.
    if (currentMillis - lastFirebaseTime >= currentConfig.interval_fb_ms)
    {
        lastFirebaseTime = currentMillis;
        firebaseUploadCycle();
    }
}

// (configureOtaRoutes removed)

void broadcastVitals()
{
    if (ws.count() == 0)
        return;

    JsonDocument doc;
    doc["type"] = "vitals";
    doc["rssi"] = WiFi.RSSI();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["min_free_heap"] = ESP.getMinFreeHeap();
    doc["uptime"] = millis() / 1000;
    doc["wifi_status"] = (WiFi.status() == WL_CONNECTED) ? "connected" : "ap_mode";

    // firebase_ready now reflects the outcome of the most recent real upload
    // attempt (see firebaseUploadCycle()), not just "Wi-Fi up + key present" —
    // that old proxy reported "ready" even though no upload had ever been
    // attempted. If the feature is off, report false rather than a stale
    // last-known state.
    doc["firebase_ready"] = currentConfig.firebase_enabled && currentVitals.firebase_ready;
    doc["firebase_last_ok_ms"] = currentVitals.firebase_last_ok_ms;
    doc["firebase_last_error"] = currentVitals.firebase_last_error;

    String payload;
    serializeJson(doc, payload);
    wsTextAllAuthed(payload);
}

void broadcastConfig()
{
    if (ws.count() == 0)
        return;

    JsonDocument doc;
    doc["type"] = "config";
    doc["wifi_ssid"] = currentConfig.wifi_ssid;
    doc["fb_api"] = currentConfig.fb_api_key;
    doc["fb_proj"] = currentConfig.fb_project;
    doc["fb_email"] = currentConfig.fb_email;
    doc["fb_col"] = currentConfig.fb_collection;
    doc["dev_id"] = currentConfig.device_id;
    doc["ph_off"] = currentConfig.ph_offset;
    doc["ph_slope"] = currentConfig.ph_slope;
    doc["tds_k"] = currentConfig.tds_k;

    // Feature flags — no reboot required for either of these.
    doc["demo"] = currentConfig.demo_mode;
    doc["fb_en"] = currentConfig.firebase_enabled;

    // Timing intervals (Part 5.8) — all three are already fully wired into
    // the firmware; this just exposes them for a "Timing" Settings card.
    doc["int_read"] = currentConfig.interval_read_ms;
    doc["int_ws"] = currentConfig.interval_ws_ms;
    doc["int_vit"] = currentConfig.interval_vitals_ms;
    doc["int_fb"] = currentConfig.interval_fb_ms;

    // Per-sensor enabled state. Order matches the SensorID enum in config.h
    // (S_WL, S_LIGHT, S_TDS, S_DHT, S_PH, S_WTEMP) — NOT the pin-array order
    // used by doc["pins"] below, which is ordered differently for historical
    // reasons. Keep these two arrays' orderings distinct and intentional.
    JsonArray sEn = doc["s_en"].to<JsonArray>();
    for (int i = 0; i < S_COUNT; i++)
        sEn.add(currentConfig.sensor_enabled[i]);

    // Send pins to JS UI for settings rendering
    // Order MUST match JS parser: TDS, DHT, pH, WaterTemp, WaterLevel, SDA, SCL, WaterLevelPower
    JsonArray pins = doc["pins"].to<JsonArray>();
    pins.add(currentConfig.pin_tds);
    pins.add(currentConfig.pin_dht);
    pins.add(currentConfig.pin_ph);
    pins.add(currentConfig.pin_ds18b20);
    pins.add(currentConfig.pin_wl);
    pins.add(currentConfig.pin_lux_sda);
    pins.add(currentConfig.pin_lux_scl);
    pins.add(currentConfig.pin_wl_power);

    String payload;
    serializeJson(doc, payload);
    wsTextAllAuthed(payload);
}

void broadcastData()
{
    if (ws.count() == 0)
        return;

    JsonDocument doc;
    doc["type"] = "data";
    doc["core_id_of_producer"] = 1; // Sensors run on core 1

    // Populated from the global currentSensors struct
    doc["tds"] = currentSensors.tds_ppm;
    doc["temp"] = currentSensors.temp_c;
    doc["hum"] = currentSensors.humidity;
    doc["w_t"] = currentSensors.water_temp_c;
    doc["lux"] = currentSensors.lux;
    doc["wl_percent"] = currentSensors.wl_percent;
    doc["ph_val"] = currentSensors.ph_val;
    doc["vpd_kpa"] = currentSensors.vpd_kpa;

    String payload;
    serializeJson(doc, payload);
    wsTextAllAuthed(payload);
}

// ----------------------------------------------------------------------------
// Server-side pin validation (Part 5.4 / 5.5)
// ----------------------------------------------------------------------------
// The client-side check in Settings (app.js) is a UX nicety only — it can be
// bypassed by a hand-crafted WS message. These two helpers are the real
// safety boundary between "the browser sent something" and "it lands in
// currentConfig/NVS". Neither one is a substitute for enforceForbiddenPins()
// in HyGrow_IoT.ino, which remains the last-resort boot-time guard.

// True if `pin` is a reserved USB D-/D+ line. -1 (disabled) is always fine.
static bool isForbiddenPin(int pin)
{
    return pin == 19 || pin == 20;
}

// Checks a proposed full set of sensor pins for GPIO19/20 use and for
// duplicate assignments between two different *enabled* sensors. -1 never
// conflicts with anything. Returns "" if the whole set is valid, or a
// human-readable reason naming the offending sensor(s)/pin if not.
struct PinCheckEntry
{
    int pin;
    const char *label;
};
static String validatePinSet(PinCheckEntry entries[], int count)
{
    for (int i = 0; i < count; i++)
    {
        if (isForbiddenPin(entries[i].pin))
        {
            return String(entries[i].label) + " is set to GPIO" + String(entries[i].pin) +
                   ", which is reserved for USB on this board.";
        }
    }

    for (int i = 0; i < count; i++)
    {
        if (entries[i].pin < 0)
            continue;
        for (int j = i + 1; j < count; j++)
        {
            if (entries[j].pin < 0)
                continue;
            if (entries[i].pin == entries[j].pin)
            {
                return String(entries[i].label) + " and " + String(entries[j].label) +
                       " are both assigned to GPIO" + String(entries[i].pin) + ".";
            }
        }
    }

    return "";
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
    if (type == WS_EVT_CONNECT)
    {
        webLog(0, LOG_INFO, "WS Client Connected: " + String(client->id()));
        // Gatekeeper: every new client starts unauthenticated, regardless of
        // whether it arrived over Wi-Fi STA or the SoftAP — AP mode does NOT
        // bypass login. The very first thing it gets is the auth_status
        // frame; broadcastConfig()/broadcastData()/broadcastVitals() below
        // are withheld from it until it sends a valid "auth" command (see
        // handleWebSocketMessage()).
        sendAuthStatus(client);
    }
    else if (type == WS_EVT_DISCONNECT)
    {
        webLog(0, LOG_INFO, "WS Client Disconnected: " + String(client->id()));
        s_authedClients.erase(client->id());
    }
    else if (type == WS_EVT_DATA)
    {
        handleWebSocketMessage(client, arg, data, len);
    }
}

// Handles { command: "auth", password/token: "..." } — the very first frame
// a client is allowed to send. Two ways to authenticate:
//  1. password — the Login/Set Password modal. If the device is
//     Unconfigured (no password set yet), any non-empty password becomes the
//     new admin password (first-time setup); otherwise it's checked against
//     the stored one.
//  2. token — silent reauth on page reload using the persisted session
//     token from localStorage, so a returning browser skips the login
//     screen entirely.
static void handleAuthCommand(AsyncWebSocketClient *client, JsonDocument &doc)
{
    String password = doc["password"] | "";
    String token = doc["token"] | "";

    bool ok = false;
    String issuedToken;

    if (token.length() > 0 && auth_check_token(token))
    {
        ok = true; // token is still the currently-valid session token — no need to reissue
    }
    else if (!auth_is_configured())
    {
        if (password.length() > 0)
        {
            auth_set_password(password);
            issuedToken = auth_issue_token();
            ok = true;
        }
    }
    else if (auth_check_password(password))
    {
        issuedToken = auth_issue_token();
        ok = true;
    }

    if (ok)
        s_authedClients.insert(client->id());

    JsonDocument resp;
    resp["type"] = "auth_result";
    resp["ok"] = ok;
    if (issuedToken.length() > 0)
        resp["token"] = issuedToken;
    String payload;
    serializeJson(resp, payload);
    client->text(payload);

    if (ok)
    {
        // Replay log history BEFORE logging this client's own auth event —
        // otherwise "WS Client N authenticated." would land in the ring
        // buffer first and then get sent to this same client twice: once
        // live (it's already in s_authedClients by this point) and once via
        // the backlog replay below. Replaying first means this client's
        // Terminal shows history strictly older than "you just connected".
        webLogSendBacklog(client);
        webLog(0, LOG_INFO, "WS Client " + String(client->id()) + " authenticated.");
        // Now that this client is trusted, give it an immediate, full
        // snapshot instead of waiting for the next broadcast tick.
        broadcastConfig();
        broadcastVitals();
        broadcastData();
    }
    else
    {
        webLog(0, LOG_WARN, "WS Client " + String(client->id()) + " failed authentication.");
    }
}

// Handles { command: "change_password", current: "...", new_pass: "..." } —
// Settings > Change Password. Requires the CURRENT password even though this
// client is already authenticated: a stolen/left-open session token alone
// shouldn't be enough to lock the real owner out of their own account.
static void handleChangePasswordCommand(AsyncWebSocketClient *client, JsonDocument &doc)
{
    String currentPass = doc["current"] | "";
    String newPass = doc["new_pass"] | "";

    JsonDocument resp;
    resp["type"] = "change_password_result";

    if (newPass.length() == 0)
    {
        resp["ok"] = false;
        resp["error"] = "New password cannot be empty.";
    }
    else if (!auth_check_password(currentPass))
    {
        resp["ok"] = false;
        resp["error"] = "Current password is incorrect.";
    }
    else
    {
        auth_set_password(newPass);
        // Re-issuing a token invalidates every previously issued token (see
        // auth_set_password()/auth_issue_token() in state.cpp) — including
        // this very client's old one — so re-authenticate THIS client
        // against the fresh token immediately rather than kicking the admin
        // out of their own change-password flow.
        String freshToken = auth_issue_token();
        s_authedClients.insert(client->id());
        resp["ok"] = true;
        resp["token"] = freshToken;
        webLog(0, LOG_INFO, "Admin password changed by client " + String(client->id()) + ".");
    }

    String payload;
    serializeJson(resp, payload);
    client->text(payload);
}

void handleWebSocketMessage(AsyncWebSocketClient *client, void *arg, uint8_t *data, size_t len)
{
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
    {
        // NOTE: deliberately no `data[len] = 0;` here. `data` points into
        // AsyncWebSocket's own receive buffer, sized exactly to `len` —
        // writing a null terminator one byte past it is an out-of-bounds
        // write. deserializeJson() is given `len` explicitly and never reads
        // past it, so the manual terminator was unnecessary and unsafe.
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);

        if (err)
        {
            webLog(0, LOG_ERR, "WS Parse Error: " + String(err.c_str()));
            return;
        }

        String cmd = doc["command"].as<String>();
        bool authed = wsClientIsAuthed(client->id());

        if (cmd == "auth")
        {
            handleAuthCommand(client, doc);
            return;
        }

        if (!authed)
        {
            // Any command other than "auth" from an unauthenticated client —
            // save_pins, reboot, request_vitals, anything — is silently
            // dropped. No error frame is sent back: an unauthenticated
            // client shouldn't learn anything about command validity either.
            webLog(0, LOG_WARN, "WS Client " + String(client->id()) + " sent '" + cmd + "' before authenticating. Dropped.");
            return;
        }

        if (cmd == "change_password")
        {
            handleChangePasswordCommand(client, doc);
            return;
        }

        if (cmd == "save_wifi")
        {
            strlcpy(currentConfig.wifi_ssid, doc["ssid"] | "", sizeof(currentConfig.wifi_ssid));
            strlcpy(currentConfig.wifi_pass, doc["pass"] | "", sizeof(currentConfig.wifi_pass));
            state_save();
            broadcastConfig();
            webLog(0, LOG_INFO, "WiFi config saved. Reboot to apply.");
        }
        else if (cmd == "save_firebase")
        {
            strlcpy(currentConfig.fb_api_key, doc["api"] | "", sizeof(currentConfig.fb_api_key));
            strlcpy(currentConfig.fb_project, doc["proj"] | "", sizeof(currentConfig.fb_project));
            strlcpy(currentConfig.fb_email, doc["email"] | "", sizeof(currentConfig.fb_email));
            strlcpy(currentConfig.fb_pass, doc["pass"] | "", sizeof(currentConfig.fb_pass));
            strlcpy(currentConfig.fb_collection, doc["col"] | "", sizeof(currentConfig.fb_collection));
            state_save();
            broadcastConfig();
            webLog(0, LOG_INFO, "Firebase config updated.");
        }
        else if (cmd == "save_pins")
        {
            // Build the proposed post-apply pin set first, so we can validate
            // it as a whole (forbidden pins + cross-sensor duplicates) BEFORE
            // committing anything to currentConfig/NVS. Reject the whole
            // command if anything is wrong — same "all or nothing" behavior
            // save_pins already had, just with a validation gate in front.
            int proposedTds = doc["pin_tds"] | currentConfig.pin_tds;
            int proposedDht = doc["pin_dht"] | currentConfig.pin_dht;
            int proposedPh = doc["pin_ph"] | currentConfig.pin_ph;
            int proposedWt = doc["pin_wt"] | currentConfig.pin_ds18b20;
            int proposedWl = doc["pin_wl"] | currentConfig.pin_wl;
            int proposedSda = doc["pin_sda"] | currentConfig.pin_lux_sda;
            int proposedScl = doc["pin_scl"] | currentConfig.pin_lux_scl;
            int proposedWlp = doc["pin_wlp"] | currentConfig.pin_wl_power;

            PinCheckEntry proposed[] = {
                {proposedTds, "TDS"},
                {proposedDht, "DHT22"},
                {proposedPh, "pH"},
                {proposedWt, "DS18B20 (Water Temp)"},
                {proposedWl, "Water Level Signal"},
                {proposedSda, "BH1750 SDA"},
                {proposedScl, "BH1750 SCL"},
                {proposedWlp, "Water Level Power"},
            };
            String problem = validatePinSet(proposed, 8);

            if (problem.length() > 0)
            {
                webLog(0, LOG_ERR, "save_pins rejected: " + problem);
                return;
            }

            currentConfig.pin_tds = proposedTds;
            currentConfig.pin_dht = proposedDht;
            currentConfig.pin_ph = proposedPh;
            currentConfig.pin_ds18b20 = proposedWt;
            currentConfig.pin_wl = proposedWl;
            currentConfig.pin_lux_sda = proposedSda;
            currentConfig.pin_lux_scl = proposedScl;
            currentConfig.pin_wl_power = proposedWlp;
            state_save();
            broadcastConfig();
            webLog(0, LOG_INFO, "Pinout config saved. Reboot required.");
        }
        else if (cmd == "calibrate_ph")
        {
            currentConfig.ph_offset = doc["offset"] | currentConfig.ph_offset;
            currentConfig.ph_slope = doc["slope"] | currentConfig.ph_slope;
            state_save();
            broadcastConfig();
            webLog(0, LOG_INFO, "pH Calibration saved.");
        }
        else if (cmd == "calibrate_tds")
        {
            currentConfig.tds_k = doc["tds_k"] | currentConfig.tds_k;
            state_save();
            broadcastConfig();
            webLog(0, LOG_INFO, "TDS Calibration saved.");
        }
        else if (cmd == "save_features")
        {
            // Any field the client omits leaves that flag unchanged — same
            // "omit-to-leave-unchanged" pattern used by save_pins. Neither
            // flag requires a reboot: both demo_mode and firebase_enabled
            // are checked live, every cycle.
            currentConfig.demo_mode = doc["demo"] | currentConfig.demo_mode;
            currentConfig.firebase_enabled = doc["fb_en"] | currentConfig.firebase_enabled;
            state_save();
            broadcastConfig();
            webLog(0, LOG_INFO, "Feature flags updated.");
        }
        else if (cmd == "save_sensor_enabled")
        {
            // Closes the Part 5.1 bug: today there's no way to flip
            // sensor_enabled[i] back to true from the UI once it's false.
            // Requires a reboot afterward (same as save_pins) since a
            // restored pin only takes effect after the sensor task
            // re-inits at boot.
            String sensor = doc["sensor"] | "";
            bool enabled = doc["enabled"] | true;
            SensorID id = S_COUNT; // sentinel; only used after matched is confirmed true
            bool matched = true;

            if (sensor == "tds")
                id = S_TDS;
            else if (sensor == "dht")
                id = S_DHT;
            else if (sensor == "ph")
                id = S_PH;
            else if (sensor == "wt")
                id = S_WTEMP;
            else if (sensor == "wl")
                id = S_WL;
            else if (sensor == "light")
                id = S_LIGHT;
            else
                matched = false;

            if (!matched)
            {
                webLog(0, LOG_ERR, "save_sensor_enabled: unknown sensor id '" + sensor + "'");
                return;
            }

            currentConfig.sensor_enabled[id] = enabled;

            // If re-enabling and the pin(s) are still -1 (e.g. from an
            // earlier auto-disable or manual pin reset), restore the
            // compiled default(s) too — otherwise the user enables a
            // sensor whose pins are still -1 and nothing happens. Reuses
            // the same defaults reset_sensor_pin() already applies.
            if (enabled)
            {
                switch (id)
                {
                case S_TDS:
                    if (currentConfig.pin_tds < 0)
                        currentConfig.pin_tds = PIN_TDS;
                    break;
                case S_DHT:
                    if (currentConfig.pin_dht < 0)
                        currentConfig.pin_dht = PIN_DHT;
                    break;
                case S_PH:
                    if (currentConfig.pin_ph < 0)
                        currentConfig.pin_ph = PIN_PH;
                    break;
                case S_WTEMP:
                    if (currentConfig.pin_ds18b20 < 0)
                        currentConfig.pin_ds18b20 = PIN_DS18B20;
                    break;
                case S_WL:
                    if (currentConfig.pin_wl < 0)
                        currentConfig.pin_wl = PIN_WL;
                    if (currentConfig.pin_wl_power < 0)
                        currentConfig.pin_wl_power = PIN_WL_PWR;
                    break;
                case S_LIGHT:
                    if (currentConfig.pin_lux_sda < 0)
                        currentConfig.pin_lux_sda = PIN_LUX_SDA;
                    if (currentConfig.pin_lux_scl < 0)
                        currentConfig.pin_lux_scl = PIN_LUX_SCL;
                    break;
                default:
                    break;
                }

                // Server-side forbidden-pin / duplicate check (Part 5.4/5.5)
                // applies here too, in case a restored default somehow
                // collides with another sensor's currently-assigned pin.
                PinCheckEntry proposed[] = {
                    {currentConfig.pin_tds, "TDS"},
                    {currentConfig.pin_dht, "DHT22"},
                    {currentConfig.pin_ph, "pH"},
                    {currentConfig.pin_ds18b20, "DS18B20 (Water Temp)"},
                    {currentConfig.pin_wl, "Water Level Signal"},
                    {currentConfig.pin_lux_sda, "BH1750 SDA"},
                    {currentConfig.pin_lux_scl, "BH1750 SCL"},
                    {currentConfig.pin_wl_power, "Water Level Power"},
                };
                String problem = validatePinSet(proposed, 8);
                if (problem.length() > 0)
                {
                    webLog(0, LOG_ERR, "save_sensor_enabled rejected: " + problem);
                    return;
                }
            }

            state_save();
            broadcastConfig();
            webLog(0, LOG_WARN, "Sensor '" + sensor + "' " + String(enabled ? "enabled" : "disabled") + ". Reboot required to apply.");
        }
        else if (cmd == "save_intervals")
        {
            // Same "omit-to-leave-unchanged" pattern as save_pins. Bounds
            // (500-60000ms) are enforced client-side in the Timing card;
            // clamp server-side too so a hand-crafted message can't set an
            // absurd interval.
            uint32_t r = doc["int_read"] | currentConfig.interval_read_ms;
            uint32_t w = doc["int_ws"] | currentConfig.interval_ws_ms;
            uint32_t v = doc["int_vit"] | currentConfig.interval_vitals_ms;
            uint32_t f = doc["int_fb"] | currentConfig.interval_fb_ms;

            auto clamp = [](uint32_t x) -> uint32_t
            { return x < 2000 ? 2000 : (x > 60000 ? 60000 : x); };

            currentConfig.interval_read_ms = clamp(r);
            currentConfig.interval_ws_ms = clamp(w);
            currentConfig.interval_vitals_ms = clamp(v);
            currentConfig.interval_fb_ms = clamp(f);
            state_save();
            broadcastConfig();
            webLog(0, LOG_INFO, "Timing intervals updated.");
        }
        else if (cmd == "reset_sensor_pin")
        {
            String sensor = doc["sensor"] | "";
            bool matched = true;

            // Part 5.1 fix: restoring the pin(s) alone used to leave
            // sensor_enabled[id] permanently false once a sensor had
            // auto-disabled (see autoDisable() in task_sensor.cpp) — nothing
            // else in the firmware ever set it back to true. Flip the flag
            // back on here, in the same place the pin gets restored, so
            // "Reset" from the Web UI actually undoes an auto-disable instead
            // of just moving the pin back while the sensor stays off forever.
            if (sensor == "tds")
            {
                currentConfig.pin_tds = PIN_TDS;
                currentConfig.sensor_enabled[S_TDS] = true;
            }
            else if (sensor == "dht")
            {
                currentConfig.pin_dht = PIN_DHT;
                currentConfig.sensor_enabled[S_DHT] = true;
            }
            else if (sensor == "ph")
            {
                currentConfig.pin_ph = PIN_PH;
                currentConfig.sensor_enabled[S_PH] = true;
            }
            else if (sensor == "wt")
            {
                currentConfig.pin_ds18b20 = PIN_DS18B20;
                currentConfig.sensor_enabled[S_WTEMP] = true;
            }
            else if (sensor == "wl")
            {
                currentConfig.pin_wl = PIN_WL;
                currentConfig.pin_wl_power = PIN_WL_PWR;
                currentConfig.sensor_enabled[S_WL] = true;
            }
            else if (sensor == "light")
            {
                currentConfig.pin_lux_sda = PIN_LUX_SDA;
                currentConfig.pin_lux_scl = PIN_LUX_SCL;
                currentConfig.sensor_enabled[S_LIGHT] = true;
            }
            else
                matched = false;

            if (matched)
            {
                state_save();
                webLog(0, LOG_WARN, "Pin(s) for '" + sensor + "' reset to compiled default and re-enabled. Rebooting...");
                delay(1000);
                ESP.restart();
            }
            else
            {
                webLog(0, LOG_ERR, "reset_sensor_pin: unknown sensor id '" + sensor + "'");
            }
        }
        else if (cmd == "factory_reset")
        {
            webLog(0, LOG_WARN, "Factory reset initiated. Wiping NVS...");
            state_factory_reset(); // This handles clear, end, and restart natively.
        }
        else if (cmd == "reboot")
        {
            webLog(0, LOG_WARN, "Manual reboot requested...");
            delay(1000);
            ESP.restart();
        }
        else if (cmd == "request_vitals")
        {
            broadcastVitals();
        }
    }
}
