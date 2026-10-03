# HyGrow app integration: local discovery and sensor status

Firmware: **3.1.4**, telemetry schema: **1**, discovery protocol: **1**.
Implementation scope is `HyGrow_IoT`; no app or AI code was changed.

## Message to the app developer

Implement a local sensor transport alongside the existing Firestore transport.
HyGrow now answers IPv4 UDP discovery on **39400** and serves read-only JSON at
**`http://<device-ip>:80/status`**. It works when Wi-Fi is connected even if the
router has no internet. Prefer a reachable local endpoint, and use Firestore
when local access is unavailable. Internet reachability and LAN reachability
must be tracked separately. Use the contract below; do not hardcode a device IP
or select the first reply when several devices answer.

The firmware samples sensors once per configured cycle. `/status`, the embedded
dashboard and Firestore consume the same completed sample. The cloud may hold
an older sample because its upload cadence differs from sensor sampling.

## 1. Find a device

Open an IPv4 UDP socket on the **Wi-Fi interface**, bind to a temporary source
port, enable broadcast, and keep that socket open for replies. Send UTF-8 JSON
to the Wi-Fi subnet's broadcast address at port **39400**. Example:
`192.168.0.255:39400` for a `192.168.0.0/24` network. Compute the actual address
from the interface IP and netmask; the network may use another range or mask.
Limited broadcast `255.255.255.255:39400` also works on networks that allow it.

```json
{"type":"hygrow_discover","version":1,"requestId":"scan_7fd94"}
```

`requestId` is required for JSON requests: **1–64 ASCII letters, digits, `_` or
`-`**. A UUID works. Optional `deviceId` filters replies to a configured device:

```json
{"type":"hygrow_discover","version":1,"requestId":"scan_7fd94","deviceId":"grow-01"}
```

Requests must fit in **256 bytes**. The exact text `Where is IoT?` is supported
for manual diagnostics; its reply has an empty `requestId`. Use JSON in the app
for correlation.

The device **unicasts** a JSON reply from UDP 39400 to your socket's source IP
and source port, through the same network interface:

```json
{
  "type": "hygrow_announce",
  "version": 1,
  "requestId": "scan_7fd94",
  "deviceId": "grow-01",
  "hardwareId": "esp32-aabbccddeeff",
  "firmwareVersion": "3.1.4",
  "ip": "192.168.0.42",
  "port": 80,
  "statusPath": "/status",
  "statusUrl": "http://192.168.0.42/status"
}
```

Collect replies for about **2 seconds**, with up to three transmissions spaced
**500 ms** apart. The device admits at most **five eligible requests per second
in total**, including malformed probes and probes for other devices. Deduplicate
by `hardwareId`; match the selected `deviceId`, echoed `requestId`, reply source
IP, port and protocol version. Fetch `/status` and verify its `deviceId` and
`hardwareId` agree before selecting that endpoint. IDs are identifiers, not
cryptographic device authentication.

`deviceId` preserves the configured Firestore document ID. If empty, it falls
back to `hardwareId`. Configure unique device IDs across physical boards.
Do not treat `hardwareId` as a human-readable name. Discovery resolves the
current address for every reply, including DHCP changes. On `HyGrow-Setup`,
the reply advertises the AP address, normally `192.168.4.1`, even if the board
also has a station connection.

## 2. Read the latest sample

```http
GET /status HTTP/1.1
Host: 192.168.0.42
```

The response is HTTP **200**, `Content-Type: application/json`, with
`Cache-Control: no-store, max-age=0`. GET and OPTIONS have read-only CORS support.
This route needs no dashboard login. It contains telemetry and identity only;
passwords, Firebase credentials, config and controls remain outside this API.

Example values below are illustrative, not measurements from a physical board:

```json
{
  "schemaVersion": 1,
  "deviceId": "grow-01",
  "hardwareId": "esp32-aabbccddeeff",
  "bootId": "c9573122a7152930",
  "firmwareVersion": "3.1.4",
  "sampleSequence": 42,
  "sampledUptimeMs": 84000,
  "sampleAgeMs": 350,
  "readIntervalMs": 2000,
  "uptime_s": 84,
  "tds_ppm": 950.25,
  "temp_c": 24.5,
  "humidity": 62,
  "water_temp_c": 22,
  "lux": 450,
  "wl_percent": 65,
  "ph_val": 6.2,
  "ph_voltage_mv": null,
  "vpd_kpa": 1.15,
  "tds_comp_using_fake_water_temp": false,
  "sensorStatus": {
    "water_level": {"enabled": true, "simulated": false, "state": "healthy"},
    "light": {"enabled": true, "simulated": false, "state": "healthy"},
    "tds": {"enabled": true, "simulated": false, "state": "healthy"},
    "dht": {"enabled": true, "simulated": false, "state": "healthy"},
    "ph": {"enabled": true, "simulated": true, "state": "healthy"},
    "water_temp": {"enabled": true, "simulated": false, "state": "healthy"}
  }
}
```

| Field | Unit / meaning |
| --- | --- |
| `tds_ppm` | TDS in ppm |
| `temp_c` | Air temperature in °C |
| `humidity` | Relative humidity in % |
| `water_temp_c` | Water temperature in °C |
| `lux` | Light level in lux |
| `wl_percent` | Water level in % |
| `ph_val` | pH |
| `ph_voltage_mv` | Additive calibration field: calibrated ADC millivolts for a healthy real pH sample; null while simulated/unavailable |
| `vpd_kpa` | Derived from DHT temperature/humidity, in kPa |

All eight display keys and the raw calibration key always exist. **Unavailable readings are `null`, never fake zero
or the previous good value.** Disabled, waiting and failed sensors give null;
nonfinite numerical values also give null. DHT failure invalidates air
temperature, humidity and VPD together. Handle zero as a valid number.

Each sensor's `state` is `disabled`, `waiting`, `healthy` or `failing`.
`simulated` distinguishes demo readings; demo readings still travel through
all transports. When `tds_comp_using_fake_water_temp` is true, real TDS used a
neutral 25 °C compensation value because usable real water temperature was
unavailable. Show that limitation with the TDS reading.

The embedded dashboard uses the sample's health and simulation metadata, keeps
one aligned 20-cycle history, and starts a new graph window on reboot or a
simulation change. Its pH wizard uses raw millivolts rather than reconstructing
voltage from a clipped pH display; simulated/stale readings cannot calibrate.
The raw field is additive under schema 1. App clients may ignore it unless they
implement calibration.

`bootId` changes on reboot. `sampleSequence` advances after each **completed**
cycle, even when all sensors are disabled or values repeat. `sampledUptimeMs`
is monotonic uptime when the cycle finished; it is not an epoch timestamp.
`sampleAgeMs` is the age of that completed cycle at HTTP response creation.
These uptime values use the ESP32's 64-bit timer.

Before the first completed cycle, `sampleSequence` and `sampledUptimeMs` are
zero, `sampleAgeMs` is null and enabled sensors report `waiting`. HTTP 200 at
startup does not imply that sensors have produced measurements yet. A stalled
sensor task can leave HTTP working; increasing sample age reveals that state.

## 3. Transport and freshness behavior to implement

1. On foreground entry or Wi-Fi change, try the cached IP with a **2-second**
   request timeout and validate identity. Rediscover if it fails or identity
   changes. Also try local access when internet is available; local availability
   must not depend on a global "internet connected" flag.
2. Poll with one request in flight at a time, no faster than
   **`max(2000, readIntervalMs)` ms**. Stop polling and discovery in the
   background. A larger configured sensor interval does not become faster
   through more HTTP requests.
3. Deduplicate samples by `(hardwareId, bootId, sampleSequence)`. Refresh
   transport freshness on successful requests, but never manufacture a new
   sample when only `uptime_s` or `sampleAgeMs` changed. Reset sequence tracking
   when `bootId` changes.
4. Treat a completed cycle as stale after a product-selected threshold; a
   starting policy is `max(10000, 3 * readIntervalMs)` ms. This threshold is an
   app recommendation, not a firmware constant. Sensor failure is separate
   from transport failure and from sample staleness.
5. After consecutive HTTP failures, rediscover and retry with capped delays
   such as 2, 4, 8 and 15 seconds. Resume normal polling after success. Don't
   scan all `192.168.*` IPs or broadcast on every sensor poll.
6. If local access is unavailable, use the existing authenticated Firestore
   transport. Mark stale cached readings explicitly when neither transport is
   fresh. The app must never send a local sensor value back to overwrite the
   device's Firestore document.

The default firmware read cadence is **2 seconds**, dashboard push cadence
**1 second**, and cloud cadence **10 seconds**. Saved settings override these
defaults. Settings about sensors can require reboot and will appear in the
published snapshot after the next completed cycle.

Firestore uses the configured collection (default `devices`) and the same
`deviceId`. Every canonical `/status` field is written atomically, plus the
existing `status: "Online"` and genuine server timestamp `lastUpdated`.
The REST typed wrappers disappear when read through a Firestore SDK. Existing
sensor field names and null behavior are preserved.

Cloud `sampleAgeMs` and `uptime_s` are values at **upload**, not at app read time.
Use `lastUpdated` for remote reachability; for sample age, add elapsed time
since `lastUpdated` to the stored `sampleAgeMs`. "Online" is the last upload's
state and cannot prove the board is still online now. Sensor status/provenance
metadata is additive; check your existing device document rules permit the
new fields without weakening owner/device access restrictions.

## 4. Mobile and web integration constraints

- **Wi-Fi with no internet:** keep local sockets/HTTP bound to Wi-Fi when the
  operating system prefers cellular for internet traffic. Both devices must
  remain on the same reachable subnet; guest isolation, a VPN or router
  broadcast filtering can prevent discovery. Keep a known-IP entry path for
  those networks. The board's boot-time recovery AP is `HyGrow-Setup` when it
  cannot join its saved Wi-Fi; the app can connect to it and read `/status`.
- **iOS/iPadOS:** add `NSLocalNetworkUsageDescription` and handle permission
  denial. Custom UDP broadcast requires the networking multicast entitlement
  in the signed app. See [Apple's local network privacy guidance](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)
  and [broadcast/multicast entitlement guidance](https://developer.apple.com/news/?id=0oi77447).
  Validate local plain HTTP against the app's App Transport Security policy on
  real devices.
- **Android:** declare `INTERNET`, support local HTTP through an appropriate
  [Network Security Configuration](https://developer.android.com/privacy-and-security/security-config),
  and handle target-dependent local network permissions. For apps targeting
  Android 17 / SDK 37+, declare and request `ACCESS_LOCAL_NETWORK` before UDP
  or HTTP LAN access. Lower targets retain implicit LAN access via `INTERNET`;
  Android 16's opt-in protection uses `NEARBY_WIFI_DEVICES`. See the current
  [Android local network permission documentation](https://developer.android.com/privacy-and-security/local-network-permission).
- **Native builds:** use a maintained native UDP/socket implementation
  compatible with the app's actual framework/version. Verify support in the
  signed build; don't assume a preview client provides the needed module or
  entitlement.
- **Browser builds:** browser JavaScript has no general raw UDP broadcast
  transport. Use an explicitly entered/discovered-through-native IP when local
  HTTP is permitted, or use Firestore. CORS alone does not override browser
  mixed-content or private/local-network restrictions.

The new endpoint is intentionally public, plain HTTP and read-only on the
reachable local network. Do not port-forward it. Device IDs and UDP replies
provide routing/selection, not trusted pairing. Authenticated dashboard
settings and commands retain their existing WebSocket login boundary.

## 5. Firmware efficiency changes and validation

- A short critical section publishes/copies a completed snapshot across cores;
  hardware reads and JSON generation happen outside that lock.
- Periodic Firebase uploads and manual connection tests run on one dedicated,
  notification-driven worker. The network loop and AsyncTCP callbacks perform
  no cloud requests. Queued periodic requests coalesce; no sensor history
  accumulates during an outage.
- Firebase tokens are cached. Credential changes are copied atomically and
  invalidate the worker's cache. Old failures cannot disable newly saved
  settings; results of manual tests that outlive settings changes are rejected.
- Transient failures preserve the Firebase-enabled preference and use a
  5, 10, 20, 40, then 60-second capped retry backoff, subject to configured
  upload cadence. Successful uploads reset it. Five consecutive permanent
  configuration/permission failures can still disable uploads and persist that
  decision. Outage retries do not write NVS.
- Discovery uses the core's asynchronous UDP implementation, bounded requests,
  same-subnet source checks and a response budget. The wildcard listener resolves
  the receiving interface afresh and retries a failed initial bind every 5s.

Run from the IoT directory:

```powershell
python tools/test-local-telemetry.py
python tools/test-local-network.py
python tools/test-cloud-worker.py
node tools/test-dashboard-flows.js
python tools/test-water-level.py
node tools/test-terminal-flows.js
node tools/test-telemetry-view.js
python tools/test-web-assets.py
pio run -e esp32-s3-n16r8
```

The host checks execute production C++ with ESP32/network primitives simulated.
They cover completed snapshot publication, JSON/cloud parity, nulls and demo
flags, request validation, rate limits, HTTP/cache/CORS, STA/AP/DHCP replies,
cloud worker coalescing, token reuse, outage recovery, configuration races,
permanent-error disable and asynchronous manual-test acknowledgements.

Version 3.1.4 needs **both a firmware upload and a LittleFS upload** to apply
the embedded dashboard repairs and terminal improvements. Regenerate gzip
files with `python tools/build-web-assets.py` before building LittleFS.
`tools/test-dashboard-browser.js` adds real DOM/browser regressions against a
local static server with mocked device traffic; it requires Playwright.
No board was flashed as part of implementation. Firmware build and
host checks do not establish radio behavior, physical sensor accuracy, live
Firestore rules or device performance. Existing cloud TLS certificate
verification is still disabled (`setInsecure`) and is outside this transport
change; this implementation does not claim to resolve that trust policy.

After flashing, use the read-only probe:

```powershell
python tools/probe-local-device.py --broadcast 192.168.0.255
python tools/probe-local-device.py --bind 192.168.0.10 --broadcast 192.168.0.255
python tools/probe-local-device.py --ip 192.168.0.42
```

On hardware, verify: router WAN disconnected while LAN remains available;
Firebase disabled and enabled; recovery after WAN returns; AP-only and AP+STA;
changed DHCP lease; two boards answering; sensor disconnect/null/demo behavior;
phone permission denial; and status/dashboard responsiveness during cloud
timeouts. Check free heap and task stack high-water marks during that soak test.
