/*
 * ============================================================================
 * task_network.h — Core 0 Task Declaration
 * ============================================================================
 */
#pragma once
#include <Arduino.h>

// The FreeRTOS task loop for networking (Runs on Core 0)
void network_task_loop(void *pvParameters);

// Called by webLog() in state.cpp to send logs to the UI Terminal
void broadcastLog(String msg);
