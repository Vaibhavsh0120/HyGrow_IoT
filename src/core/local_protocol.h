#ifndef HYGROW_LOCAL_PROTOCOL_H
#define HYGROW_LOCAL_PROTOCOL_H

#include "telemetry.h"

constexpr uint16_t HYGROW_DISCOVERY_PORT = 39400;
constexpr size_t HYGROW_DISCOVERY_MAX_REQUEST = 256;
struct DiscoveryRequest
{
    char requestId[65];
    char deviceId[32]; // optional filter; empty matches all devices
};
bool parseDiscoveryRequest(const uint8_t *data, size_t length, DiscoveryRequest &out);
void writeDiscoveryReply(JsonObject out, const DiscoveryRequest &request,
                         const TelemetrySnapshot &snapshot, const char *ip);
// Addresses in conventional big-endian numeric notation, e.g. 0xc0a80001.
bool localPeerAllowed(uint32_t peer, uint32_t local, uint32_t mask);
class DiscoveryRateLimiter
{
    bool used = false;
    uint32_t lastReplyMs = 0;
public:
    bool allow(uint32_t nowMs);
};

#endif
