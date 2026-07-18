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

// --- Web Doctor Admin Password (single-owner login) ---
// Fallback ONLY: used on first boot (blank NVS) or after the BOOT-button
// 10-second auth reset, whichever comes first. Once a password is saved via
// the Web UI's Login/Set Password overlay, it lives in NVS and this value is
// ignored — same pattern as every other FALLBACK_* credential in this file.
// Leave this empty ("") to ship "Unconfigured": the dashboard will show a
// Set Password modal on first connect instead of a Login modal.
#define FALLBACK_ADMIN_PASS ""

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
