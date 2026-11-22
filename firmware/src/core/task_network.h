#ifndef TASK_NETWORK_H
#define TASK_NETWORK_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Externally accessible web server instance
extern AsyncWebServer server;
extern AsyncWebSocket ws;

// Core task functions
void initNetworkTask();
void networkTaskLoop();

// WebSocket broadcasters
void broadcastVitals();
void broadcastConfig();
void broadcastData();

// Sends a pre-serialized JSON payload to authenticated WS clients only
// (same gate wsTextAllAuthed() uses internally for vitals/config/data).
// Exists so state.cpp's webLog() can push {"type":"log",...} frames to the
// web Terminal without needing its own copy of the auth-gate/client-loop
// logic, and without state.h having to expose the `ws` object or
// wsTextAllAuthed() itself (declared in task_network_internal.h, defined in
// websocket.cpp — internal to src/core/, not part of this public header).
// Safe to call before initNetworkTask() has run or when ws has zero
// clients — both are simple early-returns, same as the other broadcast*()
// functions above.
void wsBroadcastLog(const String &payload);

#endif // TASK_NETWORK_H
