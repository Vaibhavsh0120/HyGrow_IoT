#include "local_network.h"
#include "local_protocol.h"
#include <AsyncUDP.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <esp_netif.h>

static AsyncUDP s_discovery;
static DiscoveryRateLimiter s_discoveryBudget;
static bool s_listening = false;
static uint32_t s_lastListenAttempt = 0;

static uint32_t ipv4Number(const IPAddress &ip)
{
    return (uint32_t(ip[0]) << 24) | (uint32_t(ip[1]) << 16) |
           (uint32_t(ip[2]) << 8) | uint32_t(ip[3]);
}

static void discoveryPacket(AsyncUDPPacket packet)
{
    if (packet.isIPv6() || !packet.remotePort() ||
        packet.length() > HYGROW_DISCOVERY_MAX_REQUEST) return;
    // For broadcast packets localIP() is the broadcast destination. Resolve
    // the actual incoming interface instead, including AP+STA operation.
    const auto interface = packet.interface();
    if (interface != TCPIP_ADAPTER_IF_STA && interface != TCPIP_ADAPTER_IF_AP) return;
    auto *netif = esp_netif_get_handle_from_ifkey(interface == TCPIP_ADAPTER_IF_AP ? "WIFI_AP_DEF" : "WIFI_STA_DEF");
    esp_netif_ip_info_t info{};
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK) return;
    IPAddress local(info.ip.addr), mask(info.netmask.addr);
    if (!localPeerAllowed(ipv4Number(packet.remoteIP()), ipv4Number(local), ipv4Number(mask))) return;
    if (!s_discoveryBudget.allow(millis())) return;
    DiscoveryRequest request{};
    if (!parseDiscoveryRequest(packet.data(), packet.length(), request)) return;
    auto snapshot = telemetryRead();
    if (request.deviceId[0] && strcmp(request.deviceId, snapshot.deviceId)) return;
    JsonDocument reply;
    const String ip = local.toString();
    writeDiscoveryReply(reply.to<JsonObject>(), request, snapshot, ip.c_str());
    String payload;
    serializeJson(reply, payload);
    // AsyncUDPPacket::write unicasts to the sender's IP and source port
    // through the same interface. Never broadcast our reply to the LAN.
    packet.write(reinterpret_cast<const uint8_t *>(payload.c_str()), payload.length());
}

static void sendStatus(AsyncWebServerRequest *request)
{
    JsonDocument doc;
    const auto snapshot = telemetryRead();
    writeTelemetryJson(doc.to<JsonObject>(), snapshot, telemetryUptimeMs());
    String payload;
    serializeJson(doc, payload);
    auto *response = request->beginResponse(200, "application/json", payload);
    response->addHeader("Cache-Control", "no-store, max-age=0");
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("X-Content-Type-Options", "nosniff");
    request->send(response);
}

void localNetworkInit(AsyncWebServer &server)
{
    // Only this read-only route is public; all config and control remain
    // behind the existing WebSocket authentication gate.
    server.on("/status", HTTP_GET, sendStatus);
    server.on("/status", HTTP_OPTIONS, [](AsyncWebServerRequest *request) {
        auto *response = request->beginResponse(204);
        response->addHeader("Access-Control-Allow-Origin", "*");
        response->addHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });
    s_discovery.onPacket(discoveryPacket);
    if (s_discovery.listen(HYGROW_DISCOVERY_PORT))
    {
        s_listening = true;
        webLog(0, LOG_INFO, "Local discovery listening on UDP " + String(HYGROW_DISCOVERY_PORT) + "; sensor JSON at /status.");
    }
    else
    {
        s_lastListenAttempt = millis();
        webLog(0, LOG_WARN, "Local discovery bind failed; retrying every 5 seconds.");
    }
}

void localNetworkLoop()
{
    // A wildcard lwIP UDP listener survives STA disconnect/DHCP address
    // changes. Each reply resolves the current interface address anew.
    if (s_listening || static_cast<uint32_t>(millis() - s_lastListenAttempt) < 5000) return;
    s_lastListenAttempt = millis();
    s_listening = s_discovery.listen(HYGROW_DISCOVERY_PORT);
}
