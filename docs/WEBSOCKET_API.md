# WebSocket API Reference

Technical reference for the local `/ws` WebSocket protocol the ESP32 device
uses to talk to its own on-board dashboard (`data/index.html` + `data/js/app.js`).
This is **local-network only** — it has nothing to do with the Firestore
cloud layer documented in [`FIRESTORE_ARCHITECTURE.md`](FIRESTORE_ARCHITECTURE.md).

Audience: developers extending the dashboard or building an alternative
local client, and any AI assistant asked to modify `src/core/websocket.cpp`,
`src/core/command_handlers.cpp`, or `data/js/app.js`. End users looking for
"how do I use the dashboard" should read the main [`README.md`](../README.md)
instead — this document is protocol-level detail, not a usage guide.

---

## Connection & authentication

The Web Doctor UI communicates with the ESP32 entirely over a single
WebSocket connection at `/ws`.

Every connection starts unauthenticated — including reconnects — and must
complete a handshake before any other command is accepted. An
unauthenticated connection that sends anything other than `auth` is
silently dropped (no error frame), so it can't learn anything about command
validity either.

**Connection sequence:**

1. On connect, the server immediately sends
   `{"type": "auth_status", "setup_required": true | false}` — `true` if no
   admin password has ever been set on this device, `false` if one already
   exists.
2. The client responds with `{"command": "auth", "password": "..."}`
   (first-time setup: this also sets the password) or
   `{"command": "auth", "token": "..."}` (a previously-issued session
   token, tried silently before ever showing a login screen).
3. The server replies with
   `{"type": "auth_result", "ok": true | false, "token": "..."}`. On
   success, `token` is a fresh session token the client should store (the
   frontend keeps it in `localStorage`) and replay as step 2 on future
   connections. On failure, the client falls back to a password prompt.
4. Once authenticated, the connection immediately receives a full snapshot
   (`config`, `vitals`, `data`) rather than waiting for the next broadcast
   tick, plus any backlogged terminal log lines.

**Single remembered session:** the device holds exactly one valid session
token at a time (`s_sessionToken` in `src/core/state.cpp`), not one per
device. Logging in from a second browser/device overwrites that token — the
first device's *live* connection keeps working until it reloads or
reconnects, at which point its stored token is no longer recognized and
it's sent back to the login screen. This is intentional single-owner
behavior, not a bug.

- `{"command": "change_password", "current": "...", "new_pass": "..."}` —
  requires the current password even though the connection is already
  authenticated, so a stolen/left-open session token alone can't lock the
  real owner out. Response:
  `{"type": "change_password_result", "ok": true | false, "token": "...", "error": "..."}`.
  On success, the response's `token` re-authenticates this same connection
  against the fresh password (every other session's token, including this
  one's old value, is invalidated).

- `{"command": "logout"}` — invalidates the single stored session token
  (same reissue mechanism as `change_password`, just without setting a new
  password) and disconnects every currently-authed WS client, including
  this one, back to the login screen. This is a deliberate all-or-nothing
  logout consistent with the single-remembered-session model above: there's
  no way to log out only this browser while leaving another one's token
  valid. Response: `{"type": "logout_result", "ok": true}`. The frontend
  clears its own `localStorage` token on receiving this rather than relying
  on the reconnect to notice, so the UI returns to the login screen
  immediately.

**BOOT-button recovery:** holding the onboard BOOT button resets the admin
password/session so a lost password doesn't permanently lock a device out
(see `state.cpp` / `led_status.cpp` for the hold-duration thresholds).

---

## Command acknowledgements

Every state-changing command below (everything except `request_vitals`,
and except `reset_sensor_pin`/`reboot`/`factory_reset`, which restart the
device before a reply would matter) replies directly to the requesting
client with:

```json
{"type": "command_result", "command": "save_wifi", "ok": true}
```

or, if rejected:

```json
{"type": "command_result", "command": "save_pins", "ok": false, "error": "GPIO 19 and 20 are reserved for USB..."}
```

The frontend's save buttons wait for this ack before showing "Saved!" — the
socket being open is not the same as the device having actually applied
the change, so nothing shows success until this frame confirms it.
`test_firebase` is the one exception worth calling out: its ack is a real
network round trip on the device (Identity Toolkit sign-in + a Firestore
read), so it can take noticeably longer than every other command's
near-instant NVS write — the frontend gives it its own longer client-side
timeout for this reason.

---

## Commands (Client → ESP32)

- `{"command": "save_wifi", "ssid": "...", "pass": "..."}`
- `{"command": "save_firebase", "proj": "...", "api": "...", "email": "...", "pass": "...", "col": "..."}`
- `{"command": "save_pins", "pin_tds": 2, "pin_dht": 6, "pin_ph": 7, "pin_wt": 4, "pin_wl": 1, "pin_sda": 8, "pin_scl": 9, "pin_wlp": 5}`
  *(any field can be omitted to leave that pin unchanged; these are always
  plain GPIO numbers — a pin has no "disabled" meaning of its own, use
  `save_sensor_enabled` to turn a sensor on/off instead; requires reboot to
  apply; rejected server-side if any pin is 19/20 or duplicates another
  enabled sensor's pin)*
- `{"command": "reset_sensor_pin", "sensor": "tds" | "dht" | "ph" | "wt" | "wl" | "light"}`
  *(resets that sensor's pin(s) to the compiled default, re-enables it if
  it was auto-disabled, and reboots automatically)*
- `{"command": "save_sensor_enabled", "sensor": "tds" | "dht" | "ph" | "wt" | "wl" | "light", "enabled": true}`
  *(the ONE on/off switch per sensor — flips `sensor_enabled[i]`; the
  sensor's pin(s) are never touched by this command, so they stay exactly
  as last saved whether the sensor is on or off; requires reboot to apply)*
- `{"command": "save_features", "demo": false, "fb_en": true}`
  *(any field can be omitted to leave that flag unchanged; `fb_en` also
  resets Firebase's consecutive-failure counter when turned on; neither
  flag requires a reboot)*
- `{"command": "save_intervals", "int_read": 2000, "int_ws": 1000, "int_vit": 1000, "int_fb": 10000}`
  *(all values in ms, clamped server-side to 2000–60000; any field can be
  omitted to leave that interval unchanged)*
- `{"command": "calibrate_tds", "tds_k": 1.05, "target_ppm": 1000}`
  *(`target_ppm` is optional but sent by the TDS calibration wizard so the
  server can reject an unrealistic calibration-fluid target directly, not
  just the derived `tds_k`; both are range-checked server-side — see
  `FIRMWARE_ARCHITECTURE.md`)*
- `{"command": "calibrate_ph", "offset": 0.1, "slope": 1.02}`
- `{"command": "test_firebase"}`
  *(Settings' Test Connection button. Real connectivity check against
  whatever Firebase credentials are currently SAVED on the device — not
  whatever's currently typed in the form, so it's only meaningful after
  Save Credentials has run at least once. Signs in via Identity Toolkit and
  performs a live Firestore GET; up to two HTTPS round trips, each capped
  at 7s server-side, so this reply can take noticeably longer than other
  commands)*
- `{"command": "reboot"}`
- `{"command": "factory_reset"}` *(wipes NVS namespace and reboots into
  SoftAP mode)*
- `{"command": "request_vitals"}` *(asks the device to immediately push a
  vitals frame)*

---

## Server → Client broadcast frames

- `{"type": "config", ...}` — the full `ConfigState` snapshot (WiFi SSID,
  Firebase project/collection/device id, pin assignments, calibration
  values, feature flags, intervals). Sent on connect and after any command
  that changes it.
- `{"type": "vitals", ...}` — lightweight, frequent (`interval_vitals_ms`,
  default 1s) health frame. Includes `wifi_status` (`"connected"` |
  `"ap_mode"` — shown on the dashboard's Uplink Status tile so a user on
  the SoftAP fallback network can tell) and Firebase upload health
  (`firebase_ready`, `firebase_last_ok_ms`, `firebase_last_error` — shown
  under Cloud Provisioning's Save Credentials button) so a silently-failing
  Firestore upload doesn't go unnoticed until the mobile app's data goes
  stale.
- `{"type": "data", ...}` — the live sensor reading snapshot
  (`interval_ws_ms`, default 1s), including per-sensor `s_ok[]` health
  codes (`0`=disabled, `1`=healthy, `2`=enabled-but-erroring).
- `{"type": "log", ...}` — terminal log lines (info/warn/error), streamed
  live and backlogged across reconnects for anything logged while no
  client was connected.
