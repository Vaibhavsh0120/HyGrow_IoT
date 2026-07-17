#ifndef SECRETS_H
#define SECRETS_H

// ============================================================================
// HyGrow IoT - First Boot Credentials Fallback
// ============================================================================
// NOTE: Copy this file and rename it to 'secrets.h' before compiling.
// 'secrets.h' is in your .gitignore so it will never be uploaded to GitHub.
//
// With the new Web Doctor NVS system, these values are ONLY used if the
// device has been Factory Reset or has completely blank NVS memory.
// Once you save new credentials via the Web UI, these hardcoded values
// are safely ignored.
// ============================================================================

// --- Wi-Fi Fallback Credentials ---
#define FALLBACK_WIFI_SSID "YOUR_WIFI_SSID"
#define FALLBACK_WIFI_PASS "YOUR_WIFI_PASSWORD"

// --- SoftAP Recovery Password ---
// The password required to connect to the "HyGrow-Setup" network when STA fails
#define FALLBACK_AP_PASS "hygrowadmin"

// --- Firebase Fallback Credentials ---
#define FALLBACK_FIREBASE_API_KEY "YOUR_FIREBASE_WEB_API_KEY"
#define FALLBACK_FIREBASE_PROJECT_ID "YOUR_FIREBASE_PROJECT_ID"
#define FALLBACK_FIREBASE_USER_EMAIL "device1@yourproject.com"
#define FALLBACK_FIREBASE_USER_PASSWORD "YOUR_SECURE_PASSWORD"

// --- Firestore Default Collection ---
#define FALLBACK_FIRESTORE_COLLECTION "sensor_data"

// --- Device Identity ---
// Used to separate data if you have multiple ESP32s running this firmware
#define FALLBACK_DEVICE_ID "hygrow-node-alpha"

#endif // SECRETS_H
