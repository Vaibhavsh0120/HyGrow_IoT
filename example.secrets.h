#ifndef SECRETS_H
#define SECRETS_H

// ============================================================================
// HyGrow IoT - Required Secrets / First-Boot Credentials
// ============================================================================
// REQUIRED — the firmware will not compile without this file. Copy this
// file, rename the copy to 'secrets.h', and place it in the project root
// (same folder as HyGrow_IoT.ino). 'secrets.h' is in .gitignore so your real
// copy is never committed; config.h will fail the build with a clear #error
// if it's missing, or if any FALLBACK_* macro below isn't defined in it —
// there is no fallback-of-the-fallback baked into the firmware itself.
//
// Every value below is used ONLY on first boot (blank NVS) or after a
// Factory Reset — once you save credentials via the Web UI, they live in
// NVS from then on and these compiled-in values are ignored. But the
// firmware still needs *something* to boot with before that first save
// happens, which is exactly what this file provides — same role LittleFS's
// filesystem image plays for the web UI's static assets: a required input,
// not an optional nicety.
// ============================================================================

// --- Wi-Fi Fallback Credentials ---
#define FALLBACK_WIFI_SSID "YOUR_WIFI_SSID"
#define FALLBACK_WIFI_PASS "YOUR_WIFI_PASSWORD"

// --- SoftAP Recovery Password ---
// The password required to connect to the "HyGrow-Setup" network when STA
// fails. Must be empty ("", open network) or 8+ characters — WiFi.softAP()
// silently fails to start a WPA2 AP for anything shorter, and config.h's
// static_assert on FALLBACK_AP_PASS will refuse to compile if this is
// non-empty and under 8 characters.
#define FALLBACK_AP_PASS "hygrowadmin"

// --- Web Doctor Admin Password (single-owner login) ---
// Fallback ONLY: used on first boot (blank NVS) or after the BOOT-button
// 10-second auth reset, whichever comes first. Once a password is saved via
// the Web UI's Login/Set Password overlay, it lives in NVS and this value is
// ignored — same pattern as every other FALLBACK_* credential in this file.
// This macro must still be DEFINED even if you want that behavior — leave
// it as an explicit empty string ("") to ship "Unconfigured" (Set Password
// modal on first connect); simply omitting the #define entirely now fails
// the build rather than silently defaulting to "".
#define FALLBACK_ADMIN_PASS ""

// --- Firebase Fallback Credentials ---
#define FALLBACK_FIREBASE_API_KEY "YOUR_FIREBASE_WEB_API_KEY"
#define FALLBACK_FIREBASE_PROJECT_ID "YOUR_FIREBASE_PROJECT_ID"
#define FALLBACK_FIREBASE_USER_EMAIL "device1@yourproject.com"
#define FALLBACK_FIREBASE_USER_PASSWORD "YOUR_SECURE_PASSWORD"

// --- Firestore Default Collection ---
// One document per device lives at <collection>/<device id below> and holds
// that device's CURRENT state — this is NOT a log of historical readings.
// Change this only if you already have an existing deployment using a
// different collection name.
#define FALLBACK_FIRESTORE_COLLECTION "devices"

// --- Device Identity ---
// Used to separate data if you have multiple ESP32s running this firmware
#define FALLBACK_DEVICE_ID "hygrow-node-alpha"

// --- Demo Mode (first-boot default) ---
// true  = every sensor ships ON and generates simulated data immediately,
//         with no hardware wired up — useful for trying the dashboard, or
//         for a device that's still waiting on its sensors to arrive.
// false = normal behavior; sensors read real hardware on their configured
//         pins. This only matters on first boot / after a factory reset —
//         once Demo Mode is toggled via the Web UI, that saved value lives
//         in NVS and this compiled default is ignored, same as every other
//         FALLBACK_* value in this file.
#define FALLBACK_DEMO_MODE true

#endif // SECRETS_H
