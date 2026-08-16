# Firmware Implementation Notes

Technical detail on firmware internals that end users don't need to operate
the device, but that matter to anyone (human or AI) modifying the source.
For the Firestore/cloud layer specifically, see
[`FIRESTORE_ARCHITECTURE.md`](FIRESTORE_ARCHITECTURE.md). For the local
WebSocket protocol, see [`WEBSOCKET_API.md`](WEBSOCKET_API.md). For
end-user setup and usage, see the main [`README.md`](../README.md).

---

## 1. Dual-core task split (FreeRTOS)

- **Core 0:** WiFi, the LittleFS web server, WebSockets, NVS storage, and
  the Firestore upload cycle (`task_network.cpp`, `firebase.cpp`,
  `websocket.cpp`, `command_handlers.cpp`, `auth.cpp`).
- **Core 1:** exclusively sensor reads and Vapor Pressure Deficit (VPD)
  calculations (`task_sensor.cpp`), kept off Core 0 so network-induced
  latency (a slow Firestore round trip, a busy WebSocket) can never delay a
  timing-sensitive sensor read.

## 2. Forbidden pins: GPIO 19 & 20 — three defense layers

On the ESP32-S3, GPIO 19/20 are the native USB D-/D+ lines. This firmware
builds with `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1`, meaning
`Serial` *is* the native USB CDC peripheral on those two pins. Calling any
GPIO function on either pin fights the USB stack for the same lines —
in practice, the board appears to randomly disconnect while a serial
monitor is attached, or the upload port silently vanishes.

Three independent layers guard against this:

1. **Client-side validation** (`data/js/app.js`) — every pin field is
   checked as you type; 19/20 and duplicate pin assignments are flagged
   inline, and the Save button is disabled while any field is invalid.
   UX nicety only; bypassable by a hand-crafted WebSocket message.
2. **Server-side validation** (`save_pins`/`save_sensor_enabled` in
   `src/core/command_handlers.cpp`) — the real safety boundary. The
   proposed full pin set is checked for GPIO 19/20 and duplicate
   assignments between two *enabled* sensors before anything reaches
   `currentConfig`/NVS; the whole command is rejected with a `webLog` entry
   explaining why if either check fails.
3. **Boot guard** (`enforceForbiddenPins()`, `HyGrow_IoT.ino`) — runs
   before any sensor is touched, on every boot. If a configured pin was
   somehow saved as 19 or 20 anyway (bad manual edit, migration, a
   factory-reset race, or an older NVS blob written before layer 2
   existed), the affected sensor is force-disabled via `sensor_enabled[]`
   and persisted — the pin number itself is left as-is so it stays visible
   in Settings for correction. This is the last-resort, authoritative net.

## 3. Calibration bounds

**pH calibration wizard.** The Live Calibration page's pH card is a guided
3-step flow: Step 1 captures the 7.0 buffer point, Step 2 (only reachable
once Step 1 is done) captures the 4.0 buffer point, Step 3 (only reachable
once both points are captured) reviews and saves. A `beforeunload` handler
warns against closing/reloading mid-calibration.

**TDS calibration bounds.** Both the Live Calibration page and the server
reject unrealistic calibration-fluid targets — the accepted range is
`0`–`10000` ppm (covers deionized water through concentrated hydroponic
nutrient solution). The resulting `tds_k` scale factor is separately
bounds-checked server-side (`0 < tds_k ≤ 100`) as a second line of defense,
since a wildly wrong `tds_k` would silently corrupt every future TDS
reading (`readTDS()`, `sensor_tds.cpp`). Client-side validation lives in
`validateTdsTarget()` (`data/js/app.js`); the authoritative check is in the
`calibrate_tds` handler (`src/core/command_handlers.cpp`).

**Wi-Fi / Firebase form validation.** An empty Wi-Fi SSID is rejected
client- and server-side (an empty SSID can't be connected to, and used to
only surface ~15s after the next reboot as a confusing SoftAP fallback). A
non-empty Firebase Project ID must match Google's own project ID rules
(6–30 lowercase letters/digits/hyphens, no leading/trailing hyphen); an
empty Project ID is still allowed, since that's how Firebase provisioning
gets cleared. Client-side: `validateWifiForm()`/`validateFirebaseForm()`
(`app.js`). Server-side (authoritative): `save_wifi`/`save_firebase`
handlers (`src/core/command_handlers.cpp`).

## 4. Save reliability & crash logs

**Every settings save is verified, not assumed.** `state_save()`
(`src/core/state.cpp`) checks the return value of every
`Preferences::putX()` call — 0 bytes written means that field failed to
reach flash (a full, worn, or corrupted NVS partition). If any field
fails, `state_save()` returns `false`, and every command handler that
calls it surfaces `{"ok": false, "error": "Failed to save. Device storage
may be full or corrupted."}` instead of a blind "Saved!" ack.

**Crash/reboot reason survives to the next boot.** `esp_reset_reason()` is
read at the very start of `setup()` (`HyGrow_IoT.ino`) and persisted to its
own NVS namespace — separate from ordinary config (wiped on factory reset)
and auth (wiped on either reset type) — so a crash reason is never lost
alongside a settings wipe. On the *next* boot, the *previous* boot's reason
(e.g. `PANIC` or `TASK_WDT`) is pushed into the web Terminal's log backlog
before that boot's own reason overwrites it.

## 5. Startup validation & sensor auto-disable

Every enabled sensor is validated at boot before the normal read loop
starts: `task_sensor.cpp` attempts up to **5 reads** (250ms apart) per
sensor. If all 5 fail, that sensor's `sensor_enabled[]` flag is turned off
(pin number left untouched), persisted to NVS, and a warning is pushed to
the web terminal. The BH1750 light sensor additionally gets a bus-level I2C
presence probe (1s timeout) ahead of the retry loop, so a floating/stuck
I2C bus can't hang the sensor task and trip the Core 1 watchdog.

**Re-enabling from the Web UI:** once wiring is fixed, either click
**Reset** on that sensor's pinout card in Settings (restores the compiled
default pin(s), re-enables the sensor, reboots automatically), or flip its
**Enabled** toggle back on directly. No factory reset needed.

## 6. Per-sensor control: local reads vs. Firestore uploads

Two independent things are controlled per sensor, and it's worth being
explicit about how they relate:

1. **Whether a sensor is read at all** — `currentConfig.sensor_enabled[]`,
   set from Settings' per-sensor **Enabled** toggle. A disabled sensor is
   skipped entirely in `readAll()` (`task_sensor.cpp`).
2. **Whether that sensor's field is a real value or `null` in the
   Firestore upload** — `firebaseUploadCycle()` (`src/core/firebase.cpp`)
   sends a real `doubleValue` only when the sensor is enabled **and** its
   most recent read this boot succeeded; every other case (disabled, never
   yet read, or currently erroring) sends an explicit Firestore `null`.
   Every one of the 8 telemetry fields is listed in the write's
   `updateMask` on *every* upload — whether the value is real or `null` —
   which is what actually clears a stale value out of Firestore the moment
   a sensor stops being trustworthy, instead of leaving the last real
   reading frozen there indefinitely. See
   [`FIRESTORE_ARCHITECTURE.md`](FIRESTORE_ARCHITECTURE.md) section 2 for
   the full field-by-field contract.

**Derived fields follow their source sensor(s).** `vpd_kpa` isn't its own
sensor — it's calculated in `computeVPD()` from DHT22's temperature and
humidity. It goes `null` exactly when DHT22's `temp_c`/`humidity` do.
`uptime_s` and `firmwareVersion` are always sent as real values — they're
firmware bookkeeping, not sensor readings.

| Firestore field | Real value uploaded when... |
| --- | --- |
| `tds_ppm` | TDS sensor enabled and its last read this boot succeeded |
| `temp_c`, `humidity`, `vpd_kpa` | DHT22 enabled and its last read succeeded |
| `water_temp_c` | DS18B20 (Water Temp) enabled and its last read succeeded |
| `lux` | BH1750 (Light) enabled and its last read succeeded |
| `ph_val` | pH sensor enabled and its last read succeeded |
| `wl_percent` | Water Level sensor enabled and its last read succeeded |
| `uptime_s`, `firmwareVersion`, `deviceId`, `status` | always |

## 7. Firebase upload auto-disable after repeated failures

If five Firestore uploads in a row fail (bad/expired credentials, no
network route, a misconfigured security rule, etc.), the device
automatically turns **Firebase Upload** off
(`currentConfig.firebase_enabled = false`), persists it to NVS, and
broadcasts the change so the Settings toggle reflects it live. This exists
so a broken cloud connection fails loudly and stops retrying forever in the
background.

The failure counter (`s_fbConsecutiveFailures`, `firebase.cpp`) resets to
zero on: a successful upload, saving new Firebase credentials
(`save_firebase` — see `firebaseInvalidateToken()`), or manually flipping
Firebase Upload back on (`save_features` with `fb_en: true` — see
`firebaseResetFailureCount()`).

## 8. LED error color codes — implementation detail

`led_status.cpp` defines a color per sensor for `ledCycleErrors()`.
`task_sensor.cpp`'s read loop counts, every cycle, how many **enabled**
sensors currently have a failed last read:

- **0 failing** → LED off
- **Exactly 1 failing** → `ledCycleErrors()` shows that sensor's color, solid
- **2+ failing at once** → `ledMultiSensorFailure()` takes over: a fast
  white strobe (150ms on/off)

Disabled sensors are never counted and never shown on the LED, regardless
of their last recorded error. The boot-time-only solid magenta (LittleFS
mount failure) is never reused for a runtime sensor error — it's the only
state the LED holds solid without cycling/strobing, and only happens before
`setup()` starts either FreeRTOS task, so sensors are never initialized and
the web UI never comes up.

See the main [`README.md`](../README.md#-led-status-colors) for the
user-facing color table.
