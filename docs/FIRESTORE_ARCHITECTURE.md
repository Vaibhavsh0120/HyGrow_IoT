# Firestore Device-State Architecture

Technical reference for the `devices/{deviceId}` Firestore layer added on top
of HyGrow IoT's existing 3-pillar design (see the root `README.md` for the
pillar overview). This document is the source of truth for anyone —
human or AI — extending the Firestore, Cloud Functions, or upload-cycle
code. It intentionally goes deeper than the README, which stays focused on
what an end user needs to operate the device.

Audience: firmware/backend developers, and any AI assistant asked to modify
`src/core/firebase.cpp`, `functions/`, or `firestore.rules` in the future.

---

## 1. Why this exists

Before this change, `firebaseUploadCycle()` (`src/core/firebase.cpp`) PATCHed
a Firestore document but **omitted** any field belonging to a disabled
sensor from both the request body and the `updateMask`. Firestore's
semantics for an omitted, masked-out field are "leave whatever is already
there untouched" — so a sensor that was disabled (or auto-disabled after
failing startup validation) left its **last real reading frozen in
Firestore forever**, with nothing downstream able to tell "still reading
6.2" apart from "hasn't reported since the probe was unplugged three weeks
ago". There was also no `status`/`Online`/`Offline` concept at all — a
device that lost power looked, from Firestore's point of view, identical to
one that was still running and simply hadn't changed readings.

This document describes the fix: a single **device-state document per
device**, always written as a complete, self-consistent snapshot, plus a
small backend service that turns silence into an explicit `Offline` status.

---

## 2. Data model

### 2.1 Collection & document

```
devices/{deviceId}
```

One document per physical device. `{deviceId}` is `currentConfig.device_id`
(`state.h`) — configured per board via `secrets.h`'s `FALLBACK_DEVICE_ID` on
first boot / factory reset, and persisted to NVS (`dev_id` key) from then
on. This is what makes multiple devices coexist safely: `hygrow_001`,
`hygrow_002`, `hygrow_003`, etc. each get their own document, and nothing in
the firmware, rules, or Functions code hardcodes a single device.

The collection name itself defaults to **`devices`** (`fb_collection` in
`ConfigState`, `DEFAULT_FB_COLLECTION` in `config.h`, overridable at runtime
from Settings > Cloud Provisioning, same as before this change). It was
`sensor_readings` prior to this change — renamed because the document is a
**current-state mirror**, not a log of historical readings (that distinction
is the whole point of Pillar 2 in the README's architecture section). If you
already have a live deployment using the old name, either keep your
`fb_collection` set to `sensor_readings` (nothing else breaks) or rename the
collection in Firestore and update `firestore.rules`'s `match /devices/...`
line to match.

### 2.2 Field reference

Written by `firebaseUploadCycle()` (`src/core/firebase.cpp`), one write per
`currentConfig.interval_fb_ms` (default 10s, Settings > Timing):

| Field | Type | Always present? | Meaning |
|---|---|---|---|
| `deviceId` | string | yes | Mirrors `currentConfig.device_id`. Also mirrors the document's own path segment (enforced by `firestore.rules`). |
| `status` | string | yes | `"Online"` — written by the ESP32 on every successful upload. **Never** written as `"Offline"` by the firmware; see §4. |
| `lastUpdated` | timestamp | yes | Firestore **server** timestamp (via a field transform, not the device's clock — see §3). Refreshed on every successful upload. |
| `uptime_s` | integer | yes | `millis()/1000` — device-side bookkeeping, not used for Online/Offline logic. |
| `firmwareVersion` | string | yes | `FIRMWARE_VERSION` macro, `src/core/firebase.cpp`. Bump by hand when firmware behavior changes meaningfully. |
| `temp_c`, `humidity` | double \| null | yes (value or null) | DHT22. Both null together iff `S_DHT` unavailable. |
| `vpd_kpa` | double \| null | yes (value or null) | Derived from `temp_c`/`humidity` (`computeVPD()`, `task_sensor.cpp`). Null whenever they are — it has no independent sensor of its own. |
| `water_temp_c` | double \| null | yes (value or null) | DS18B20, gated on `S_WTEMP`. |
| `tds_ppm` | double \| null | yes (value or null) | Gated on `S_TDS`. |
| `lux` | double \| null | yes (value or null) | BH1750, gated on `S_LIGHT`. |
| `ph_val` | double \| null | yes (value or null) | Gated on `S_PH` (ships disabled by default — see README). |
| `wl_percent` | double \| null | yes (value or null) | Gated on `S_WL`. |
| `offlineDetectedAt` | timestamp | only while Offline | Backend-only bookkeeping — see §4.2. Absent while a device is Online. |

That's **8 telemetry fields** (`temp_c`, `humidity`, `vpd_kpa`, `water_temp_c`,
`tds_ppm`, `lux`, `ph_val`, `wl_percent`) plus 4 device-bookkeeping fields
(`deviceId`, `status`, `lastUpdated`, `uptime_s`) plus `firmwareVersion`.

### 2.3 Availability → null, not omission, not zero

`firebaseUploadCycle()` decides per sensor with:

```cpp
auto sensorAvailable = [](SensorID id) -> bool {
    return currentConfig.sensor_enabled[id] &&
           currentSensors.last_ok_ms[id] != 0 &&
           currentSensors.last_err[id][0] == '\0';
};
```

A sensor is only "available" (real `doubleValue` sent) if it is **enabled
AND has a clean most-recent read this boot**. Every other case — disabled,
never yet successfully read, or currently erroring — sends an explicit
Firestore `nullValue`. This is a real, distinct JSON `null`
(`{"nullValue": null}`), not the field being absent and not `0` (which
would be indistinguishable from a legitimate sensor reading of zero — e.g.
`wl_percent: 0` is a real, valid empty-tank reading).

Every one of the 8 telemetry fields is **always** listed in the write's
`updateMask`, whether the value is real or null — see §3 for why this
matters. This is the actual fix for the pre-existing bug: a field that goes
from valid to unavailable is now guaranteed to be overwritten with `null` on
the very next upload cycle, not left frozen at its last value.

`demo_mode` (Settings > Feature Flags) behaves identically for this
purpose — `readAllDemo()` (`task_sensor.cpp`) calls the same `markOk()` path
real reads use, so `sensorAvailable()` can't tell the difference and demo
data uploads exactly like real data would.

---

## 3. Why `lastUpdated` needs a server timestamp, and why that requires `commit`, not `patch`

Offline detection (§4) only works if `lastUpdated` cannot be spoofed by a
misbehaving or compromised client, and cannot silently be wrong because the
ESP32's own clock is unset/drifted (this board has no RTC battery — its
clock is whatever NTP or nothing has told it since boot). Both problems have
the same fix: never trust a client-supplied timestamp value; use Firestore's
own **server value transform** (`setToServerValue: REQUEST_TIME`) instead.

This turned out to matter for which REST endpoint the firmware calls, not
just which JSON key it sends — worth recording since it's easy to get
subtly wrong (an earlier version of this exact file tried to send a
`timestampValue` string and silently got a useless literal stored instead,
per the `NOTE` this replaced in `firebase.cpp`'s history):

- **`PATCH .../documents/{collection}/{docId}`** (`projects.databases.documents.patch`)
  accepts a request body that is only a plain `Document` — `{"fields": {...}}`.
  There is **no field-transform mechanism available on this endpoint at
  all**. A `timestampValue` sent here is a literal, client-supplied value —
  exactly the thing this design needs to avoid.
- **`POST .../documents:commit`** (`projects.databases.documents.commit`)
  accepts a `{"writes": [Write, ...]}` body, where each `Write` supports
  `update` (a `Document`), `updateMask`, **and** `updateTransforms` — a list
  of `FieldTransform`s, each of which can set a field to
  `setToServerValue: "REQUEST_TIME"`. Firestore applies `updateTransforms`
  immediately after `update` in the same write, atomically.

`firebaseUploadCycle()` therefore uses `documents:commit` with a single
`Write` entry: an ordinary field update (deviceId/status/uptime/firmwareVersion/
the 8 telemetry fields) plus one `updateTransforms` entry for `lastUpdated`.
`lastUpdated` is deliberately **not** included in either `updateMask` or the
plain `fields{}` body — it exists only as the transform's target.

```jsonc
POST https://firestore.googleapis.com/v1/projects/{project}/databases/(default)/documents:commit?key={apiKey}
{
  "writes": [
    {
      "updateMask": { "fieldPaths": ["deviceId", "status", "uptime_s", "firmwareVersion",
                                       "tds_ppm", "temp_c", "humidity", "vpd_kpa",
                                       "water_temp_c", "lux", "ph_val", "wl_percent"] },
      "updateTransforms": [
        { "fieldPath": "lastUpdated", "setToServerValue": "REQUEST_TIME" }
      ],
      "update": {
        "name": "projects/{project}/databases/(default)/documents/devices/{deviceId}",
        "fields": { "...": "..." }
      }
    }
  ]
}
```

This is also exactly what `firestore.rules` relies on to validate the write
without trusting the client (§5): `request.resource.data.lastUpdated ==
request.time` is only true when the value genuinely came from a server-value
transform for *this* request — Firestore rules evaluate `request.resource`
*after* transforms are resolved, so a rule can compare the transformed
result against `request.time` directly. A hand-crafted request that tries to
set `lastUpdated` as a plain field would have to predict the server's own
processing timestamp to the millisecond to pass this check, which isn't
practically possible.

---

## 4. Online / Offline semantics

**This is the single most important design rule in this document:**
`status` describes device **connectivity**, never sensor health.

> ESP32 connected + pH sensor failed → `status: "Online"`, `ph_val: null`
> ESP32 disconnected (any reason) → `status: "Offline"` (eventually — see below), whatever sensor values it last had

### 4.1 Online: written by the device, on every successful upload

`firebaseUploadCycle()` sets `status: "Online"` unconditionally on every
commit that reaches Firestore successfully — the write **reaching**
Firestore at all already proves the device currently has both Wi-Fi and a
working path to Firestore, which is the entire definition of "Online" here.
It does not depend on any sensor's state.

### 4.2 Offline: written only by the backend, never by the device

The ESP32 **never** writes `status: "Offline"`. It can't, meaningfully — a
device that has lost connectivity has no way to tell Firestore it's down
(that's precisely what makes it "down"). Offline is therefore entirely a
**backend-derived** state: `functions/index.js`'s `checkDeviceHeartbeats`
runs on a schedule, finds every document currently claiming `status ==
"Online"`, and flips any whose `lastUpdated` is older than
`STALE_THRESHOLD_MS` (30 000 ms) to `"Offline"`, stamping a
diagnostic-only `offlineDetectedAt` field via `FieldValue.serverTimestamp()`.
It does **not** touch `lastUpdated` itself — that field must keep reflecting
the last time the *device* genuinely reported in, so a client app can still
show an accurate "last seen 4 minutes ago".

`markDeviceOnlineOnWrite` (the second function in `functions/index.js`) is a
small backstop that clears `offlineDetectedAt` once a device's own write
confirms `status: "Online"` again after being marked stale — it does **not**
independently decide Online/Offline itself (that would create two sources of
truth for the same field); the device's own upload already set `status`
correctly by the time this trigger runs.

### 4.3 The 30-second threshold vs. Cloud Scheduler's real floor — an honest limitation

The requirement, as specified, is "Offline within 30 seconds of the last
heartbeat". **Cloud Scheduler — the mechanism every Firebase `onSchedule`
function is built on — cannot be configured to run more often than once per
minute.** There is no way to get a genuine 30-second polling loop out of a
serverless scheduled function; the only way to check more often than that
would be an always-on process (a min-instance Cloud Run service, a VM, etc.),
which is a materially different (and non-free) architecture than "the
minimum required Firebase Functions structure" the task asked for.

Given that constraint, this implementation makes a deliberate choice rather
than silently drifting from the spec:

- `STALE_THRESHOLD_MS` stays at the requested **30 000 ms** — a device is
  considered stale the instant its `lastUpdated` is more than 30s old,
  exactly as specified, evaluated against `Timestamp.now()` each time the
  function runs.
- The scheduled function itself runs **every 60 seconds** (`CHECK_SCHEDULE`
  in `functions/index.js`) — Cloud Scheduler's actual floor.

**Net effect:** a device that goes dark is marked Offline somewhere between
roughly 30 and 90 seconds after its last real heartbeat, depending on where
in the 1-minute polling cycle it happened to stop — not a hard 30-second
guarantee. If a tighter bound is ever genuinely required, the options are:
(a) a min-instance-1 Cloud Run/Functions service running its own in-process
setInterval loop (removes the "serverless" property and adds a fixed
monthly cost), or (b) a Realtime Database `onDisconnect()` presence system
running alongside Firestore, which *can* detect disconnects within seconds
because it relies on the client's live socket rather than polling — a
meaningfully larger architecture change than what this task asked for.

### 4.4 Multi-device safety

`checkDeviceHeartbeats` queries `where('status', '==', 'Online')` across the
whole `devices` collection (a single-field equality query, which Firestore
indexes automatically — no composite index needed, hence
`firestore.indexes.json` is intentionally empty) and batches every resulting
update into one `WriteBatch`. This scales the same way regardless of how
many `devices/{deviceId}` documents exist, and never touches a document that
is already `"Offline"` (avoiding needless repeated writes to devices that
are already known to be down).

---

## 5. Security model

Full rules: `firestore.rules`, with inline comments mirroring this section.
Summary:

| Actor | Read | Write |
|---|---|---|
| Public / app (no auth) | ✅ allowed | ❌ denied |
| Authenticated Firebase user (device credentials) | ✅ allowed | ✅ allowed, but constrained — see below |
| Cloud Functions (Admin SDK) | ✅ | ✅ — bypasses rules entirely (documented Firebase behavior for server client libraries) |

A client write (`create`/`update`) is only accepted if **all** of:
1. `request.auth != null` — blocks every unauthenticated request outright.
2. The written keys are exactly the known device-state field set (defense
   in depth — a client can't smuggle extra fields into the document).
3. `deviceId` in the body matches the document's own path segment.
4. `status` is written as exactly `"Online"` — a client-side write can
   never set `"Offline"` through this rule, full stop.
5. `lastUpdated` equals `request.time` — i.e. it must be a genuine server
   timestamp transform (§3), not a client-supplied value.

`delete` is denied unconditionally for every client — decommissioning a
device is a console/Admin-SDK operation.

### 5.1 The gap this project does **not** close, and why

This firmware authenticates via plain Firebase email/password sign-in
(Identity Toolkit) — the same mechanism available to *any* client in the
project, not a per-device certificate or attestation. Firestore rules see
"an authenticated Firebase user", not "the specific ESP32 board this
document belongs to". Nothing in the firmware, NVS config, or rules above
maps a specific `request.auth.uid` to a specific `deviceId` — so, in
principle, anyone holding valid credentials for *any* HyGrow device account
in this project can write telemetry to *any* `devices/{deviceId}` document,
not only the one matching their own physical board.

This is a genuine, acknowledged limitation of building on the existing
architecture (single-owner email/password auth, direct ESP32 → Firestore
REST calls) rather than something papered over with a blanket `allow read,
write: if true;` rule. Two ways to close it, neither implemented here
because both are a larger architectural change than this task's scope:

1. **Per-device custom claims.** Give each physical device its own Firebase
   Auth account and set a custom claim on it (e.g.
   `{"deviceId": "hygrow_001"}` via the Admin SDK, done once out-of-band —
   custom claims can't be self-assigned by the client). Then add
   `&& request.auth.token.deviceId == deviceId` to
   `isValidDeviceWrite()` in `firestore.rules`.
2. **Proxy writes through an authenticated Cloud Function.** Instead of the
   ESP32 writing to Firestore directly, it would `POST` to an `onRequest`
   Cloud Function (with its own auth check), and the function — using the
   Admin SDK, which bypasses rules — would be the only thing that ever
   writes to `devices/{deviceId}`, deciding server-side which `deviceId` a
   given request is allowed to touch. This also removes the Firestore API
   key and Identity Toolkit credentials from the firmware image entirely,
   at the cost of an extra network hop per upload cycle.

If your deployment has a real multi-tenant threat model (mutually
distrusting device owners sharing one Firebase project), implement one of
the above before relying on this ruleset. For a single owner running their
own hydroponics devices under their own project (the situation this
firmware ships for), the existing constraint — valid Firebase Auth
credentials, `status` locked to `"Online"`, `lastUpdated` locked to the
server clock — is what "as much as practical" means in this codebase today.

---

## 6. Deployment

One-time setup (Firebase Console or CLI — either works):

1. **Enable Firestore** in Native mode, if not already enabled, in the
   Firebase Console for your project.
2. **Create the Firebase Auth account(s)** the device(s) will sign in as
   (Console → Authentication → Users → Add user), matching whatever you put
   in `secrets.h`'s `FALLBACK_FIREBASE_USER_EMAIL`/`_PASSWORD` (or later
   change via Settings > Cloud Provisioning). Email/Password sign-in must be
   enabled as a provider.
3. Fill in `firebase/.firebaserc` with your real project ID (replace
   `YOUR_FIREBASE_PROJECT_ID`).
4. `firebase.json`/`.firebaserc`/`firestore.rules` all live inside the
   `firebase/` folder (not the repo root), since that folder is this
   project's self-contained Firebase CLI workspace. From `firebase/`:
   ```bash
   cd firebase
   npm install -g firebase-tools   # if you don't already have the CLI
   firebase login
   npm --prefix functions install  # installs firebase-admin/firebase-functions
   firebase deploy --only firestore:rules,functions
   ```
   This deploys `firestore.rules` and both functions in
   `functions/index.js`. The Cloud Scheduler API is enabled automatically
   the first time a scheduled function deploys (`checkDeviceHeartbeats`) —
   if your Google Cloud project has never used Cloud Scheduler before, the
   CLI will prompt you to confirm/enable billing (Cloud Functions v2 and
   Cloud Scheduler both require the Blaze pay-as-you-go plan; the free-tier
   quota comfortably covers a small number of devices checked once a
   minute).
5. Flash the firmware with `fb_collection` left as its default (`devices`)
   or set to whatever you deployed the rules against, and Firebase Upload
   enabled (Settings > Feature Flags) with valid credentials (Settings >
   Cloud Provisioning). Use **Test Connection** to confirm reachability
   before relying on the automatic upload cycle.
6. Watch `devices/{your device_id}` appear in the Firestore Console within
   one `interval_fb_ms` cycle (default 10s) of the device connecting.

To test the Offline path without physically cutting power: turn off
**Firebase Upload** in Settings, or disconnect the device's Wi-Fi, and watch
`status` flip to `"Offline"` in the Firestore Console within roughly a
minute (see §4.3 for exactly why it's "roughly" and not instant).

---

## 7. Files in this architecture

| File | Role |
|---|---|
| `src/core/firebase.cpp` | Builds and sends the `devices/{deviceId}` commit every upload cycle. |
| `firebase/functions/index.js` | `checkDeviceHeartbeats` (Offline detector) + `markDeviceOnlineOnWrite` (reconnect cleanup). |
| `firebase/functions/package.json` | Cloud Functions dependencies (`firebase-admin`, `firebase-functions`) and `npm run deploy`/`serve`/`lint` scripts. |
| `firebase/functions/eslint.config.js` | Minimal flat ESLint config for `functions/`. |
| `firebase/firestore.rules` | Security rules implementing §5. |
| `firebase/firestore.indexes.json` | Intentionally empty — the only query (`where('status','==','Online')`) is a single-field equality query, auto-indexed by Firestore. |
| `firebase/firebase.json` | Firebase CLI project config (rules path, functions source dir, emulator ports). |
| `firebase/.firebaserc` | Project ID alias — fill in before deploying. |
| `docs/FIRESTORE_ARCHITECTURE.md` | This file. |
