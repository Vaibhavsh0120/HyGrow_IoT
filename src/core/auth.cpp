// ----------------------------------------------------------------------------
// auth.cpp — single-owner WebSocket login.
// ----------------------------------------------------------------------------
// Split out of the original task_network.cpp (see task_network_internal.h
// for the full map of the split). Owns:
//   - the "is this WS client logged in?" tracking (s_authedClients)
//   - the initial auth_status frame sent on every new connection
//   - the "auth" command (password or session-token login)
//   - the "change_password" command
// No functional changes from the original — this is a structural split only.
// ----------------------------------------------------------------------------
#include "task_network.h"
#include "task_network_internal.h"
#include "state.h"
#include <set>

// ----------------------------------------------------------------------------
// WebSocket auth gatekeeper (single-owner login)
// ----------------------------------------------------------------------------
// AsyncWebSocketClient has no free field to flag "authenticated", so
// authenticated client ids are tracked in their own set here instead. A
// client's id is unique for the lifetime of its connection (AsyncWebSocket
// assigns a fresh one per connect), and wsForgetClient() (called from
// websocket.cpp's WS_EVT_DISCONNECT) removes it, so this set never grows
// unbounded and never confuses one browser tab's session with another's.
static std::set<uint32_t> s_authedClients;

bool wsClientIsAuthed(uint32_t clientId)
{
    return s_authedClients.find(clientId) != s_authedClients.end();
}

void wsMarkClientAuthed(uint32_t clientId)
{
    s_authedClients.insert(clientId);
}

void wsForgetClient(uint32_t clientId)
{
    s_authedClients.erase(clientId);
}

// Sends the very first frame a client sees on connect: whether the device
// still needs its first password set ("setup_required": true) or already has
// one and is waiting for a login ("setup_required": false). The frontend
// (data/js/app.js) branches its overlay purely off this flag.
void sendAuthStatus(AsyncWebSocketClient *client)
{
    JsonDocument doc;
    doc["type"] = "auth_status";
    doc["setup_required"] = !auth_is_configured();
    String payload;
    serializeJson(doc, payload);
    client->text(payload);
}

// Handles { command: "auth", password/token: "..." } — the very first frame
// a client is allowed to send. Two ways to authenticate:
//  1. password — the Login/Set Password modal. If the device is
//     Unconfigured (no password set yet), any non-empty password becomes the
//     new admin password (first-time setup); otherwise it's checked against
//     the stored one.
//  2. token — silent reauth on page reload using the persisted session
//     token from localStorage, so a returning browser skips the login
//     screen entirely.
void handleAuthCommand(AsyncWebSocketClient *client, JsonDocument &doc)
{
    String password = doc["password"] | "";
    String token = doc["token"] | "";

    bool ok = false;
    String issuedToken;

    if (token.length() > 0 && auth_check_token(token))
    {
        ok = true; // token is still the currently-valid session token — no need to reissue
    }
    else if (!auth_is_configured())
    {
        if (password.length() > 0)
        {
            auth_set_password(password);
            issuedToken = auth_issue_token();
            ok = true;
        }
    }
    else if (auth_check_password(password))
    {
        issuedToken = auth_issue_token();
        ok = true;
    }

    if (ok)
        wsMarkClientAuthed(client->id());

    JsonDocument resp;
    resp["type"] = "auth_result";
    resp["ok"] = ok;
    if (issuedToken.length() > 0)
        resp["token"] = issuedToken;
    String payload;
    serializeJson(resp, payload);
    client->text(payload);

    if (ok)
    {
        // Replay log history BEFORE logging this client's own auth event —
        // otherwise "WS Client N authenticated." would land in the ring
        // buffer first and then get sent to this same client twice: once
        // live (it's already authed by this point) and once via the
        // backlog replay below. Replaying first means this client's
        // Terminal shows history strictly older than "you just connected".
        webLogSendBacklog(client);
        webLog(0, LOG_INFO, "WS Client " + String(client->id()) + " authenticated.");
        // Now that this client is trusted, give it an immediate, full
        // snapshot instead of waiting for the next broadcast tick.
        broadcastConfig();
        broadcastVitals();
        broadcastData();
    }
    else
    {
        webLog(0, LOG_WARN, "WS Client " + String(client->id()) + " failed authentication.");
    }
}

// Handles { command: "change_password", current: "...", new_pass: "..." } —
// Settings > Change Password. Requires the CURRENT password even though this
// client is already authenticated: a stolen/left-open session token alone
// shouldn't be enough to lock the real owner out of their own account.
void handleChangePasswordCommand(AsyncWebSocketClient *client, JsonDocument &doc)
{
    String currentPass = doc["current"] | "";
    String newPass = doc["new_pass"] | "";

    JsonDocument resp;
    resp["type"] = "change_password_result";

    if (newPass.length() == 0)
    {
        resp["ok"] = false;
        resp["error"] = "New password cannot be empty.";
    }
    else if (!auth_check_password(currentPass))
    {
        resp["ok"] = false;
        resp["error"] = "Current password is incorrect.";
    }
    else
    {
        auth_set_password(newPass);
        // Re-issuing a token invalidates every previously issued token (see
        // auth_set_password()/auth_issue_token() in state.cpp) — including
        // this very client's old one — so re-authenticate THIS client
        // against the fresh token immediately rather than kicking the admin
        // out of their own change-password flow.
        String freshToken = auth_issue_token();
        wsMarkClientAuthed(client->id());
        resp["ok"] = true;
        resp["token"] = freshToken;
        webLog(0, LOG_INFO, "Admin password changed by client " + String(client->id()) + ".");
    }

    String payload;
    serializeJson(resp, payload);
    client->text(payload);
}
