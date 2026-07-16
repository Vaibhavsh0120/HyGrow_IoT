/*
 * ============================================================================
 *  firebase_handler.cpp — Firebase Firestore Communication Implementation
 * ============================================================================
 *  
 *  Uses the FirebaseClient library (Mobizt) for async Firestore operations.
 *  
 *  Library: https://github.com/mobizt/FirebaseClient
 *  Install: Arduino IDE → Sketch → Include Library → Manage Libraries
 *           → Search "FirebaseClient" by Mobizt
 * ============================================================================
 */

#include "firebase_handler.h"
#include "firebase_handler.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>

// Required for FirebaseClient v2.x to enable these modules
#define ENABLE_USER_AUTH
#define ENABLE_FIRESTORE
#include <FirebaseClient.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Firebase Objects
// ─────────────────────────────────────────────────────────────────────────────

// WiFi & SSL
static WiFiClientSecure sslClient;

// Authentication
static UserAuth userAuth(FIREBASE_API_KEY, FIREBASE_USER_EMAIL, FIREBASE_USER_PASSWORD);

// Firebase App
using AsyncClient = AsyncClientClass;
static AsyncClient aClient(sslClient);

static FirebaseApp app;
static Firestore::Documents Docs;

// State tracking
static bool _firebaseReady     = false;
static bool _initialAuthDone   = false;
static unsigned long _lastSendTime = 0;

// Async result for monitoring
static AsyncResult firestoreResult;

// ─────────────────────────────────────────────────────────────────────────────
//  WiFi Connection
// ─────────────────────────────────────────────────────────────────────────────

static bool connectWiFi() {
    DBGLN("[WIFI] Connecting to: " + String(WIFI_SSID));

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startTime > WIFI_CONNECT_TIMEOUT_MS) {
            DBGLN("[WIFI] ERROR: Connection timed out!");
            return false;
        }
        DBG(".");
        delay(WIFI_RETRY_DELAY_MS);
    }

    DBGLN("");
    DBGLN("[WIFI] Connected!");
    DBGLN("[WIFI] IP: " + WiFi.localIP().toString());
    DBGF("[WIFI] RSSI: %d dBm\n", WiFi.RSSI());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Get NTP time string in RFC3339 format for Firestore timestamps
// ─────────────────────────────────────────────────────────────────────────────

static String getTimestampString() {
    // Use NTP time if available, otherwise use millis-based placeholder
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 1000)) {
        char buf[30];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
        return String(buf);
    }
    // Fallback — return epoch-based timestamp
    return "1970-01-01T00:00:00Z";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public Implementation
// ─────────────────────────────────────────────────────────────────────────────

bool firebaseInit() {
    // Step 1: Connect WiFi
    if (!connectWiFi()) {
        return false;
    }

    // Step 2: Configure NTP for timestamps
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    DBGLN("[FIREBASE] NTP time sync started");

    // Step 3: SSL setup — skip certificate verification for simplicity
    // For production, use proper root CA certificates
    sslClient.setInsecure();

    // Step 4: Initialize Firebase
    DBGLN("[FIREBASE] Initializing with project: " + String(FIREBASE_PROJECT_ID));

    initializeApp(aClient, app, getAuth(userAuth), firestoreResult);

    // Wait for authentication
    unsigned long authStart = millis();
    while (!app.ready() && millis() - authStart < FIREBASE_READY_TIMEOUT_MS) {
        firebaseLoop();
        delay(10);
    }
    
    // Bind Firestore Documents to the app
    app.getApp<Firestore::Documents>(Docs);

    if (!app.ready()) {
        DBGLN("[FIREBASE] ERROR: Authentication failed or timed out!");
        return false;
    }

    _firebaseReady = true;
    _initialAuthDone = true;
    DBGLN("[FIREBASE] ✓ Authenticated and ready!");
    return true;
}

bool firebaseSendData(SensorData &data) {
    if (!_firebaseReady || !app.ready()) {
        DBGLN("[FIREBASE] Not ready — skipping send");
        return false;
    }

    // Check WiFi
    if (WiFi.status() != WL_CONNECTED) {
        DBGLN("[FIREBASE] WiFi disconnected — attempting reconnect");
        if (!connectWiFi()) {
            return false;
        }
    }

    // Build the Firestore document using FirebaseClient's native Document class
    String timestamp = getTimestampString();
    
    Values::IntegerValue waterLevelRawVal(data.waterLevelRaw);
    Values::DoubleValue  waterLevelPctVal(number_t(data.waterLevelPercent, 2));
    Values::DoubleValue  lightLuxVal(number_t(data.lightLux, 2));
    Values::DoubleValue  tdsPPMVal(number_t(data.tdsPPM, 2));
    Values::DoubleValue  airTempVal(number_t(data.airTempC, 2));
    Values::DoubleValue  humidityVal(number_t(data.humidityPercent, 2));
    Values::DoubleValue  vpdVal(number_t(data.vpdKpa, 2));
    Values::DoubleValue  phVal(number_t(data.phValue, 2));
    Values::DoubleValue  waterTempVal(number_t(data.waterTempC, 2));
    Values::StringValue  deviceIdVal(String(DEVICE_ID));
    Values::TimestampValue timestampVal(timestamp);
    
    Document<Values::Value> doc("water_level_raw", Values::Value(waterLevelRawVal));
    doc.add("water_level_percent", Values::Value(waterLevelPctVal));
    doc.add("light_lux", Values::Value(lightLuxVal));
    doc.add("tds_ppm", Values::Value(tdsPPMVal));
    doc.add("air_temp_c", Values::Value(airTempVal));
    doc.add("humidity_percent", Values::Value(humidityVal));
    doc.add("vpd_kpa", Values::Value(vpdVal));
    doc.add("ph_value", Values::Value(phVal));
    doc.add("water_temp_c", Values::Value(waterTempVal));
    doc.add("device_id", Values::Value(deviceIdVal));
    doc.add("timestamp", Values::Value(timestampVal));

    // Update or Create exactly ONE document so we don't fill the database infinitely
    String documentPath = String(FIRESTORE_COLLECTION) + "/" + DEVICE_ID;

    Docs.patch(aClient,
               Firestore::Parent(FIREBASE_PROJECT_ID),
               documentPath,
               PatchDocumentOptions(DocumentMask(), DocumentMask(), Precondition()),
               doc,
               firestoreResult);

    DBGF("[FIREBASE] → Sending to '%s' (ts: %s)\n",
         FIRESTORE_COLLECTION, timestamp.c_str());

    return true;
}

void firebaseLoop() {
    // Process Firebase async tasks
    app.loop();

    // Maintain auth token
    if (_initialAuthDone && app.ready()) {
        _firebaseReady = true;
    }

    // Check for async results
    if (firestoreResult.isResult()) {
        if (firestoreResult.isError()) {
            DBGF("[FIREBASE] Async ERROR: %s (code: %d)\n",
                 firestoreResult.error().message().c_str(),
                 firestoreResult.error().code());
            // Don't set _firebaseReady to false here — transient errors are normal
        }
        firestoreResult.clear();
    }
}

bool isFirebaseReady() {
    return _firebaseReady && app.ready();
}

int getWiFiRSSI() {
    return WiFi.RSSI();
}
