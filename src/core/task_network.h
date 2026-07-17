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

#endif // TASK_NETWORK_H
