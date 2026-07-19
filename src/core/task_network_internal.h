#ifndef TASK_NETWORK_INTERNAL_H
#define TASK_NETWORK_INTERNAL_H

// ----------------------------------------------------------------------------
// task_network_internal.h — private glue between the 5 files task_network.cpp
// used to be a single 1000+-line file. NOT a public header: nothing outside
// src/core/ includes this (compare task_network.h, which IS the public
// surface — initNetworkTask()/networkTaskLoop()/broadcast*()/wsBroadcastLog()).
//
// The split, and which file owns what:
//   task_network.cpp     — task lifecycle (initNetworkTask/networkTaskLoop)
//                           + broadcastVitals()/broadcastConfig()/broadcastData()
//   auth.cpp              — single-owner login: s_authedClients tracking,
//                           sendAuthStatus(), handleAuthCommand(),
//                           handleChangePasswordCommand()
//   firebase.cpp           — Firestore token exchange + firebaseUploadCycle()
//   websocket.cpp          — wsTextAllAuthed(), onWsEvent(),
//                           handleWebSocketMessage() (the top-level frame
//                           parser/router)
//   command_handlers.cpp   — sendCmdAck(), pin validation helpers, and
//                           handleDeviceCommand() (every command branch that
//                           isn't "auth"/"change_password": save_wifi,
//                           save_firebase, save_pins, calibrate_ph,
//                           calibrate_tds, save_features,
//                           save_sensor_enabled, save_intervals,
//                           reset_sensor_pin, factory_reset, reboot,
//                           request_vitals)
//
// Purely a structural split — no functionality changed from the original
// single-file implementation.
// ----------------------------------------------------------------------------
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// ---------- websocket.cpp ----------
// Sends `payload` only to clients that have completed the auth handshake.
// Every broadcast*() in task_network.cpp, and webLog()'s WS frame (via
// wsBroadcastLog() in task_network.h), goes through this one choke point.
void wsTextAllAuthed(const String &payload);
// AsyncWebSocket's connect/disconnect/data event callback, registered on
// `ws` in initNetworkTask().
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
// Parses one WS_EVT_DATA frame as JSON and routes it: "auth" -> auth.cpp,
// "change_password" -> auth.cpp, everything else (once authenticated) ->
// command_handlers.cpp's handleDeviceCommand().
void handleWebSocketMessage(AsyncWebSocketClient *client, void *arg, uint8_t *data, size_t len);

// ---------- auth.cpp ----------
// Single-owner WS login gatekeeper. See auth.cpp for the full contract.
bool wsClientIsAuthed(uint32_t clientId);
void wsMarkClientAuthed(uint32_t clientId);
void wsForgetClient(uint32_t clientId); // called from websocket.cpp's WS_EVT_DISCONNECT
void sendAuthStatus(AsyncWebSocketClient *client);
void handleAuthCommand(AsyncWebSocketClient *client, JsonDocument &doc);
void handleChangePasswordCommand(AsyncWebSocketClient *client, JsonDocument &doc);

// ---------- firebase.cpp ----------
// Fires one Firestore PATCH with the current sensor snapshot, at most once
// per currentConfig.interval_fb_ms. Called from networkTaskLoop().
void firebaseUploadCycle();
// Clears the cached Identity Toolkit ID token so the next upload cycle signs
// in fresh. Call this any time fb_email/fb_pass/fb_project/fb_api_key change
// — see save_firebase in command_handlers.cpp.
void firebaseInvalidateToken();

// ---------- command_handlers.cpp ----------
// Sends a per-command acknowledgement directly to the requesting client only
// (never broadcast) — see the definition in command_handlers.cpp for the
// full contract the frontend's sendCommand()/handleCommandResult() (app.js)
// depends on.
void sendCmdAck(AsyncWebSocketClient *client, const String &cmd, bool ok, const String &error = "");
// Every command other than "auth"/"change_password" — save_wifi,
// save_firebase, save_pins, calibrate_ph, calibrate_tds, save_features,
// save_sensor_enabled, save_intervals, reset_sensor_pin, factory_reset,
// reboot, request_vitals. Only ever called once the client has passed auth
// (see the gate in handleWebSocketMessage(), websocket.cpp).
void handleDeviceCommand(AsyncWebSocketClient *client, const String &cmd, JsonDocument &doc);

#endif // TASK_NETWORK_INTERNAL_H
