#ifndef HYGROW_LOCAL_NETWORK_H
#define HYGROW_LOCAL_NETWORK_H
class AsyncWebServer;
void localNetworkInit(AsyncWebServer &server);
void localNetworkLoop(); // retry a failed UDP bind, never blocks on internet
#endif
