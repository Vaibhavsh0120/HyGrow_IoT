/*
 * ============================================================================
 *  firebase_handler.h — Firebase Firestore Communication Handler
 * ============================================================================
 *  
 *  Handles WiFi connection, Firebase authentication (email/password),
 *  and Firestore document creation using the FirebaseClient library by Mobizt.
 *  
 *  This module operates asynchronously — firebaseLoop() must be called
 *  in every iteration of loop() for proper operation.
 * ============================================================================
 */

#ifndef FIREBASE_HANDLER_H
#define FIREBASE_HANDLER_H

#include <Arduino.h>
#include "../../config.h"

/**
 * Initialize WiFi and Firebase authentication.
 * Blocks until WiFi is connected and Firebase is authenticated.
 * @return true if both WiFi and Firebase are ready.
 */
bool firebaseInit();

/**
 * Send all sensor data to Firestore as a new document.
 * Creates a document with auto-generated ID in the configured collection.
 * @param data  Reference to a SensorData struct with all sensor readings.
 * @return true if the send was initiated successfully.
 */
bool firebaseSendData(SensorData &data);

/**
 * Process Firebase async tasks. MUST be called in every loop() iteration.
 */
void firebaseLoop();

/**
 * Check if Firebase is authenticated and ready to send data.
 * @return true if ready.
 */
bool isFirebaseReady();

/**
 * Get the current WiFi RSSI (signal strength).
 */
int getWiFiRSSI();

#endif // FIREBASE_HANDLER_H
