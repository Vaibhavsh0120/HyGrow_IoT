// ----------------------------------------------------------------------------
// websocket.cpp — AsyncWebSocket event/message dispatcher.
// ----------------------------------------------------------------------------
// Split out of the original task_network.cpp (see task_network_internal.h
// for the full map of the split). Owns:
//   - wsTextAllAuthed(): the one choke point every broadcast/log frame goes
//     through, so nothing reaches a client before it authenticates
//   - onWsEvent(): AsyncWebSocket's connect/disconnect/data callback
//   - handleWebSocketMessage(): parses each frame and routes "auth" /
//     "change_password" to auth.cpp, everything else to
//     command_handlers.cpp's handleDeviceCommand()
// No functional changes from the original — this is a structural split only.
// ----------------------------------------------------------------------------
#include "task_network.h"
#include "task_network_internal.h"
#include "state.h"

// Sends `payload` only to clients that have completed the auth handshake —
// used by every broadcast*() function in task_network.cpp instead of
// ws.textAll(), so an unauthenticated connection (pre-login, or one that
// never logs in at all) never receives live telemetry, config, or vitals
// data, regardless of whether it arrived over Wi-Fi STA or the SoftAP.
void wsTextAllAuthed(const String &payload)
{
    for (AsyncWebSocketClient &c : ws.getClients())
    {
        if (c.status() == WS_CONNECTED && wsClientIsAuthed(c.id()))
        {
            c.text(payload);
        }
    }
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
    if (type == WS_EVT_CONNECT)
    {
        webLog(0, LOG_INFO, "WS Client Connected: " + String(client->id()));
        // Gatekeeper: every new client starts unauthenticated, regardless of
        // whether it arrived over Wi-Fi STA or the SoftAP — AP mode does NOT
        // bypass login. The very first thing it gets is the auth_status
        // frame; broadcastConfig()/broadcastData()/broadcastVitals() are
        // withheld from it until it sends a valid "auth" command (see
        // handleWebSocketMessage() below).
        sendAuthStatus(client);
    }
    else if (type == WS_EVT_DISCONNECT)
    {
        webLog(0, LOG_INFO, "WS Client Disconnected: " + String(client->id()));
        wsForgetClient(client->id());
    }
    else if (type == WS_EVT_DATA)
    {
        handleWebSocketMessage(client, arg, data, len);
    }
}

void handleWebSocketMessage(AsyncWebSocketClient *client, void *arg, uint8_t *data, size_t len)
{
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
    {
        // NOTE: deliberately no `data[len] = 0;` here. `data` points into
        // AsyncWebSocket's own receive buffer, sized exactly to `len` —
        // writing a null terminator one byte past it is an out-of-bounds
        // write. deserializeJson() is given `len` explicitly and never reads
        // past it, so the manual terminator was unnecessary and unsafe.
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);

        if (err)
        {
            webLog(0, LOG_ERR, "WS Parse Error: " + String(err.c_str()));
            return;
        }

        String cmd = doc["command"].as<String>();
        bool authed = wsClientIsAuthed(client->id());

        if (cmd == "auth")
        {
            handleAuthCommand(client, doc);
            return;
        }

        if (!authed)
        {
            // Any command other than "auth" from an unauthenticated client —
            // save_pins, reboot, request_vitals, anything — is silently
            // dropped. No error frame is sent back: an unauthenticated
            // client shouldn't learn anything about command validity either.
            webLog(0, LOG_WARN, "WS Client " + String(client->id()) + " sent '" + cmd + "' before authenticating. Dropped.");
            return;
        }

        if (cmd == "change_password")
        {
            handleChangePasswordCommand(client, doc);
            return;
        }

        // Every remaining command name (save_wifi, save_pins, calibrate_*,
        // factory_reset, reboot, request_vitals, ...) lives in
        // command_handlers.cpp.
        handleDeviceCommand(client, cmd, doc);
    }
}
