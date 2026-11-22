#include "local_protocol.h"

bool parseDiscoveryRequest(const uint8_t *data, size_t length, DiscoveryRequest &out)
{
    out = {};
    if (!data || !length || length > HYGROW_DISCOVERY_MAX_REQUEST) return false;
    constexpr char humanProbe[] = "Where is IoT?";
    if (length == sizeof(humanProbe) - 1 && !memcmp(data, humanProbe, length)) return true;
    // ArduinoJson stops at the closing object. Require a single complete
    // object with no trailing datagrams/garbage or embedded binary NULs.
    bool quoted = false, escaped = false, ended = false;
    int depth = 0;
    for (size_t i = 0; i < length; ++i)
    {
        const char c = static_cast<char>(data[i]);
        if (!c) return false;
        if (ended)
        {
            if (c != ' ' && c != '\r' && c != '\n' && c != '\t') return false;
            continue;
        }
        if (quoted)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        }
        else if (c == '"') quoted = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) ended = true;
    }
    if (!ended || quoted || depth) return false;
    JsonDocument doc;
    if (deserializeJson(doc, data, length, DeserializationOption::NestingLimit(2))) return false;
    if (!doc.is<JsonObject>() || !doc["type"].is<const char *>() ||
        strcmp(doc["type"].as<const char *>(), "hygrow_discover") ||
        !doc["version"].is<unsigned>() || doc["version"].as<unsigned>() != 1 ||
        !doc["requestId"].is<const char *>()) return false;
    const char *requestId = doc["requestId"];
    const size_t idLength = doc["requestId"].as<JsonString>().size();
    if (idLength != strlen(requestId)) return false;
    if (!idLength || idLength >= sizeof(out.requestId)) return false;
    for (size_t i = 0; i < idLength; ++i)
    {
        const char c = requestId[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    const JsonVariantConst filterValue = doc["deviceId"];
    if (!filterValue.isNull())
    {
        if (!doc["deviceId"].is<const char *>()) return false;
        const char *filter = doc["deviceId"];
        if (!filter[0] || strlen(filter) >= sizeof(out.deviceId) ||
            strlen(filter) != filterValue.as<JsonString>().size()) return false;
        strlcpy(out.deviceId, filter, sizeof(out.deviceId));
    }
    strlcpy(out.requestId, requestId, sizeof(out.requestId));
    return true;
}

void writeDiscoveryReply(JsonObject out, const DiscoveryRequest &request,
                         const TelemetrySnapshot &snapshot, const char *ip)
{
    out["type"] = "hygrow_announce";
    out["version"] = 1;
    out["requestId"] = request.requestId;
    out["deviceId"] = snapshot.deviceId;
    out["hardwareId"] = snapshot.hardwareId;
    out["firmwareVersion"] = HYGROW_FIRMWARE_VERSION;
    out["ip"] = ip;
    out["port"] = 80;
    out["statusPath"] = "/status";
    char url[40];
    snprintf(url, sizeof(url), "http://%s/status", ip);
    out["statusUrl"] = url;
}

bool localPeerAllowed(uint32_t peer, uint32_t local, uint32_t mask)
{
    if (!local || !mask || !peer || (peer & mask) != (local & mask)) return false;
    const uint32_t host = peer & ~mask;
    return host != 0 && host != ~mask && peer != local;
}

bool DiscoveryRateLimiter::allow(uint32_t nowMs)
{
    if (used && static_cast<uint32_t>(nowMs - lastReplyMs) < 200) return false;
    used = true;
    lastReplyMs = nowMs;
    return true;
}
