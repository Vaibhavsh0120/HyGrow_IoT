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

// ----------------------------------------------------------------------------
// Failed-password lockout
// ----------------------------------------------------------------------------
// handleAuthCommand() below used to have no limit on failed password
// attempts at all -- an attacker with WebSocket access (e.g. connected to
// the SoftAP) could brute-force guess indefinitely, and auth_check_password()
// only recently became constant-time (see state.cpp), which on its own does
// nothing to stop a guessing loop. This is a single global counter rather
// than one per client id: AsyncWebSocket hands out a fresh client id per
// TCP connection, so a per-client counter would reset to zero on every
// reconnect and provide no real protection. There's exactly one admin
// account on this device, so a single device-wide counter is the right
// scope -- it also means one attacker can't dodge the lockout by opening
// multiple connections in parallel.
//
// Backoff grows with each consecutive failure (1s, 2s, 4s, ... capped at
// 30s) rather than a hard lockout window, so a legitimate user who
// mistypes their password a couple of times barely notices, while a
// scripted guessing loop gets slowed to a crawl. The counter resets to
// zero on any successful auth.
static uint16_t s_failedAuthAttempts = 0;
static unsigned long s_authLockoutUntilMs = 0;

static const uint16_t AUTH_LOCKOUT_MAX_BACKOFF_MS = 30000;

// Doubles the backoff per consecutive failure: 1s, 2s, 4s, 8s, 16s, 30s
// (capped), 30s, 30s... Called only after a failed attempt is recorded.
static unsigned long authBackoffForAttempt(uint16_t attempts)
{
    if (attempts == 0)
        return 0;
    unsigned long ms = 1000UL << (attempts - 1 > 15 ? 15 : attempts - 1); // guard against shift overflow
    return ms > AUTH_LOCKOUT_MAX_BACKOFF_MS ? AUTH_LOCKOUT_MAX_BACKOFF_MS : ms;
}

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

    // Lockout only applies to password guesses, not token reauth. A stale
    // or invalid token is a normal, frequent occurrence (e.g. every
    // browser tab that was open across a password change) and isn't
    // evidence of anyone guessing anything -- gating it the same way
    // would let a few routine reconnects lock a legitimate user out of
    // their own device.
    bool isPasswordAttempt = password.length() > 0 && !(token.length() > 0 && auth_check_token(token));
    unsigned long now = millis();

    if (isPasswordAttempt && s_authLockoutUntilMs != 0 && (long)(s_authLockoutUntilMs - now) > 0)
    {
        JsonDocument resp;
        resp["type"] = "auth_result";
        resp["ok"] = false;
        resp["error"] = "Too many failed attempts. Please wait before trying again.";
        String payload;
        serializeJson(resp, payload);
        client->text(payload);
        webLog(0, LOG_WARN, "WS Client " + String(client->id()) + " auth attempt rejected: locked out.");
        return;
    }

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
    {
        wsMarkClientAuthed(client->id());
        // Any successful auth clears the failure streak -- a legitimate
        // login shouldn't stay throttled because of earlier mistyped
        // attempts, and this is also what lets the counter reset for the
        // one real admin instead of accumulating forever.
        s_failedAuthAttempts = 0;
        s_authLockoutUntilMs = 0;
    }
    else if (isPasswordAttempt)
    {
        if (s_failedAuthAttempts < 60000) // guard against wraparound over an extremely long uptime
            s_failedAuthAttempts++;
        s_authLockoutUntilMs = now + authBackoffForAttempt(s_failedAuthAttempts);
    }

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
