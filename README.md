# HyGrow-IoT: ESP32-S3 Hydroponics Monitor

![ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-blue)
![FreeRTOS](https://img.shields.io/badge/OS-FreeRTOS-orange)
![Firebase](https://img.shields.io/badge/Database-Firestore-yellow)
![Status](https://img.shields.io/badge/Status-Active-success)

A compact ESP32-S3 firmware for monitoring hydroponic and environmental data from six sensors. It serves a local dashboard from LittleFS, acts as a fault-tolerant edge device, and can be built with either Arduino IDE, Arduino CLI, or PlatformIO.

---

## 🏛️ System Architecture Context

HyGrow is built on a strictly decoupled, 3-pillar architecture. This codebase represents **Pillar 3** only.

*   **Pillar 1: The React Native App & Hugging Face AI (The Brain)**
    *   Handles all the heavy lifting, historical data tracking, push notifications, and predictive plant health models via Hugging Face.
*   **Pillar 2: Firebase / Firestore (The Bridge)**
    *   Acts purely as a real-time state mirror holding current, live sensor values.
*   **Pillar 3: The ESP32-S3 IoT Device (The Edge / This Codebase)**
    *   Operates purely as a fault-tolerant **"dumb pipe"** and a local configuration appliance.
    *   **NO** historical data storage.
    *   **NO** predictive analytics or complex hydroponic logic.
    *   Its only jobs are to accurately read analog/digital sensors, push that raw data to Firestore in real-time, and host a local "router-style" Web UI for hardware setup, Wi-Fi provisioning, and sensor calibration.

---

## 🌟 Key Features

- **Dual-Core Processing (FreeRTOS):**
  - **Core 0:** Handles WiFi, the LittleFS Web Server, WebSockets, NVS storage, and asynchronous Firebase uploads.
  - **Core 1:** Exclusively dedicated to precise hardware timing, sensor reads, and Vapor Pressure Deficit (VPD) calculations without network-induced latency.
- **Offline-First "Stitch" UI:** A premium, responsive "liquid-glass" Single Page Application hosted directly on the ESP32's flash memory. No internet or external CDNs required.
- **True Offline Fallback:** If the configured WiFi fails, the ESP32 automatically broadcasts a `HyGrow-Setup` Access Point (SoftAP) for local configuration and diagnostics.
- **Dynamic NVS Configuration (Web Doctor):** Update WiFi credentials, Firebase keys, sensor GPIO pins, feature flags, and interval timings directly from the web dashboard. Settings persist across reboots via Non-Volatile Storage (NVS) with self-healing recovery logic.
- **Feature Flags:** Demo Mode (simulate all six sensors for testing the dashboard with no hardware wired up) and a Firebase Upload master switch — all live-toggleable from Settings with no reboot required.
- **Guided Sensor Calibration:** Calibrate pH via a guided 2-point wizard, and TDS via a 1-point target wizard, calculated entirely in the browser UI.
- **Startup Validation & Auto-Disable:** Every enabled sensor gets 5 boot-time read attempts before the system trusts it; a sensor that fails all 5 is automatically disabled. Fully re-enabling it is a single click from the Web UI.
- **Light / Dark / Auto Theme:** The dashboard theme is switchable from Settings and persisted per-browser; "Auto" follows the OS `prefers-color-scheme`.
- **RGB Status LED:** The onboard WS2812 NeoPixel shows solid green while every enabled sensor is healthy, and cycles through a per-sensor error color whenever an enabled sensor's most recent read failed. Also provides a fatal blinking Red/Magenta error for flash mounting failures.

---

## 🛠 Hardware & Wiring

**Supported Board:** ESP32-S3 N16R8 DevKit

This project is optimized for modular sensor kits that include built-in resistors and signal conditioning (such as terminal adapters and pull-ups). This allows for **direct connection** to the ESP32-S3 without requiring additional breadboard circuitry.

### Specific Sensor Hardware Used

| Sensor Module & Purchase Link                                                                                                      | Protocol | ESP32-S3 Pin (Default)\*      | Notes                                                              |
| ---------------------------------------------------------------------------------------------------------------------------------- | -------- | ------------------------------ | ------------------------------------------------------------------ |
| **[Water Level Sensor](https://amzn.in/d/0cKf4nuQ)**                                                                               | Analog   | GPIO 1 (Sig) / GPIO 5 (Pwr)    | Power-gated: only energized for ~10ms per read to drastically reduce electrolytic corrosion. |
| **[BH1750 Light Sensor](https://amzn.in/d/09NZHxCq)**                                                                              | I2C      | GPIO 8 (SDA) / GPIO 9 (SCL)    | Digital ambient light detection. Probed for a live I2C ACK at boot. |
| **[DFRobot Gravity Analog TDS](https://robocraze.com/products/dfrobot-gravity-analog-tds-water-quality-sensor-meter-for-arduino)** | Analog   | GPIO 2                         | Uses median filtering in code for noise reduction.                 |
| **[Hexonix DHT22 AM2302](https://amzn.in/d/07a1dbpF)**                                                                             | Digital  | GPIO 6                         | Temperature & Humidity. Module includes built-in pull-up resistor.|
| **[DFRobot Gravity Lab pH V2](https://robu.in/product/dfrobot-gravity-lab-grade-analog-ph-sensor-meter-kit-v2/)**                  | Analog   | GPIO 7                         | Lab-grade analog pH sensing. Native 3.3V support!                  |
| **[amiciSense DS18B20 Kit](https://amzn.in/d/0exQsfGD)**                                                                           | OneWire  | GPIO 4                         | Waterproof temp probe. Kit includes the terminal adapter.          |
| **Built-in RGB LED**                                                                                                               | NeoPixel | GPIO 48                        | Onboard WS2812 used for system health visualization.               |

_\*Note: GPIO pins can now be dynamically reassigned in the Web UI Settings tab. The table above lists the compiled fallback defaults from `config.h`, used on first boot or after a factory reset._

> **⚡ Power Note:** Thanks to the V2 specifications of the DFRobot modules, **every single sensor in this project shares a unified 3.3V and GND rail**. No 5V logic-level shifting is required!

> **🔌 Default Enabled State:** On first boot (and after a factory reset), every sensor ships **enabled** except **pH**, which ships **disabled**. pH needs a probe calibrated in real liquid to read anything meaningful, so it's off until you've calibrated it and turn it on yourself — from the pH sensor's detail page (**Enable Power**) or its pinout card in Settings (**Enabled** toggle, under Sensor Implementation Config). Its pin still defaults to GPIO 7 either way, so turning it on doesn't require re-wiring, just a reboot to apply.

### ⚠️ Forbidden Pins: GPIO 19 & GPIO 20

**Never assign any sensor, LED, or other peripheral to GPIO 19 or GPIO 20.**

On the ESP32-S3, these two pins are the native USB D-/D+ lines. This firmware builds with `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` (see `platformio.ini`), which means `Serial` **is** the native USB CDC peripheral, and it lives on GPIO 19/20. Calling `pinMode()`, `analogRead()`, `digitalWrite()`, or any other GPIO function on either pin fights the USB stack for the same lines. In practice this shows up as the board appearing to randomly disconnect from your computer while a serial monitor is attached, or the upload port silently vanishing.

The firmware defends against this in three layers:

1. **Client-side validation (Settings tab, `data/js/app.js`):** every pin field is checked the moment you type — 19/20 is flagged instantly with an inline warning naming the offending field, and duplicate pin assignments across two different sensors are flagged the same way. The Save button is disabled while any field is invalid. This is a UX nicety only — it runs in the browser and can be bypassed by a hand-crafted WebSocket message.
2. **Server-side validation (`save_pins` / `save_sensor_enabled` handlers in `src/core/task_network.cpp`):** the real safety boundary. Before anything is written to `currentConfig`/NVS, the proposed full pin set is checked for GPIO 19/20 and for duplicate assignments between two enabled sensors; the whole command is rejected (with a `webLog(LOG_ERR, ...)` explaining why) if either check fails.
3. **Boot guard (`enforceForbiddenPins()` in `HyGrow_IoT.ino`):** runs before any sensor is touched, on every boot. If any configured pin was somehow saved as 19 or 20 anyway (a bad manual edit, a migration, a factory-reset race, or an older NVS blob written before layer 2 existed), it is forced back to `-1` (disabled) and the change is persisted to NVS. This is the last-resort, authoritative safety net and is never weakened or removed by the two layers above.

### Startup Validation & Auto-Disable

Every enabled sensor is validated at boot before the system settles into its normal read loop: `task_sensor.cpp` attempts up to **5 reads** (250ms apart) per sensor. If all 5 attempts fail, that sensor's pin(s) are set to `-1`, its feature flag is turned off, the change is saved to NVS, and a warning is pushed to the web terminal explaining why. This keeps a genuinely unwired or dead sensor from spamming "read failed" into the terminal forever.

**Re-enabling it from the Web UI:** once the wiring is fixed, either click **Reset** on that sensor's pinout card in Settings (restores the compiled default pin(s) **and** clears the auto-disable flag, then reboots automatically), or flip its **Enabled** toggle back on (sends `save_sensor_enabled`, which also restores the pin(s) if they're still `-1`, then prompts you to reboot). Either path fully undoes the auto-disable — you do **not** need a factory reset just to bring one sensor back.

The BH1750 light sensor additionally gets a bus-level check ahead of the 5x retry: `sensor_light.cpp` performs a bounded I2C presence probe (ACK check, 1s timeout) before ever calling into the BH1750 library, so a floating/stuck I2C bus can't hang the sensor task and trip the Core 1 watchdog.

### Per-Sensor Control: Local Reads vs. Firestore Uploads

Two independent things are controlled per sensor, and it's worth being explicit about how they relate:

1. **Whether a sensor is read at all** — `currentConfig.sensor_enabled[]`, set from Settings' per-sensor **Enabled** toggle (or the sensor detail page's **Enable Power** toggle). A disabled sensor is skipped entirely in `readAll()` (`task_sensor.cpp`) — its `currentSensors` value just holds whatever it last read (or 0, if never read this boot) and stops updating.
2. **Whether that sensor's field is included in the Firestore upload** — gated the same way, on the same six `sensor_enabled[]` flags, inside `firebaseUploadCycle()` (`task_network.cpp`). A field for a disabled sensor is left out of both the PATCH request body **and** its `updateMask.fieldPaths`, so Firestore doesn't touch that field on the existing document at all — it's not overwritten with a stale local value, and it's not left silently frozen at whatever it was uploaded as before the sensor was turned off.

In other words: turning a sensor off in Settings stops it being read locally, and — as of the same toggle — also stops its field from being pushed to Firestore on the next upload cycle. You don't need a separate "send to cloud" switch per sensor; the one **Enabled** toggle covers both.

**Derived fields follow their source sensor(s).** `vpd_kpa` (Vapor Pressure Deficit) isn't its own sensor — it's calculated in `computeVPD()` from DHT22's temperature and humidity readings. It's gated on the DHT22 **Enabled** flag specifically: turn DHT22 off and `temp_c`, `humidity`, and `vpd_kpa` are all dropped from the upload together, since VPD can't be meaningfully computed without the temperature/humidity it depends on. `uptime_s` and `server_timestamp` are always sent — they're firmware bookkeeping, not sensor readings.

| Firestore field | Uploaded when... |
| --- | --- |
| `tds_ppm` | TDS sensor enabled |
| `temp_c`, `humidity`, `vpd_kpa` | DHT22 sensor enabled (VPD is derived from these two) |
| `water_temp_c` | DS18B20 (Water Temp) sensor enabled |
| `lux` | BH1750 (Light) sensor enabled |
| `ph_val` | pH sensor enabled |
| `wl_percent` | Water Level sensor enabled |
| `uptime_s`, `server_timestamp` | always |

---

## ⚙️ Arduino IDE Board Settings (ESP32-S3 N16R8)

To successfully flash this firmware, configure your Arduino IDE **Tools** menu exactly as follows:

- **Board:** ESP32S3 Dev Module
- **USB CDC On Boot:** Enabled
- **CPU Frequency:** 240MHz (WiFi)
- **Flash Mode:** QIO 80MHz
- **Flash Size:** 16MB (128Mb)
- **Partition Scheme:** 16M Flash (3MB APP/9.9MB FATFS) _(Required to fit the compiled Tailwind UI in LittleFS)_
- **PSRAM:** OPI PSRAM
- **Upload Mode:** UART0 / Hardware CDC

### PlatformIO Equivalent Settings

The PlatformIO environment in [platformio.ini](platformio.ini) is configured to match the same board profile:

- `src_dir = .` so PlatformIO builds the root Arduino sketch [HyGrow_IoT.ino](HyGrow_IoT.ino)
- `default_envs = esp32-s3-n16r8`
- `board = esp32-s3-devkitc-1`
- `build_flags` enable USB CDC on boot
- `board_build.flash_mode = qio`
- `board_build.f_cpu = 240000000L`
- `board_upload.flash_size = 16MB`
- `lib_deps` uses `ESP32Async/AsyncTCP` and `ESP32Async/ESPAsyncWebServer` for the async web stack

If you want an Arduino IDE build to match exactly, keep the board profile and the settings above aligned with the README section.

---

## 🚀 Quick Start

### 1. Pick a build path

- **Arduino IDE:** open [HyGrow_IoT.ino](HyGrow_IoT.ino) and build with the board settings below.
- **Arduino CLI:** install `arduino-cli`, then compile the sketch with the same ESP32-S3 options shown below.
- **PlatformIO:** open the folder in VS Code and use the single environment named `esp32-s3-n16r8`.

### 2. Install Arduino libraries

If you are using the Arduino IDE or Arduino CLI, install these libraries. Versions below are **pinned to the exact set verified working** on the `esp32-s3-n16r8` PlatformIO environment (via `platformio pkg list`) — match them in Arduino's Library Manager where possible to avoid API drift:

| Library                  | Author            | Verified Version | Notes                              |
| ------------------------ | ------------------ | ----------------- | ----------------------------------- |
| ESPAsyncWebServer         | ESP32Async         | 3.11.2             | "ESP Async WebServer" in Lib Manager |
| AsyncTCP                  | ESP32Async         | 3.4.10              | "Async TCP" in Lib Manager           |
| ArduinoJson               | Benoit Blanchon    | 7.4.3               |                                      |
| Adafruit NeoPixel         | Adafruit           | 1.15.5              |                                      |
| Adafruit Unified Sensor   | Adafruit           | 1.1.15              | Dependency of DHT sensor library     |
| DHT sensor library        | Adafruit           | 1.4.7                |                                      |
| DallasTemperature         | Miles Burton       | 3.11.0               |                                      |
| OneWire                   | Paul Stoffregen    | 2.3.8                | Dependency of DallasTemperature      |
| BH1750                    | Christopher Laws   | 1.3.0                 |                                      |

PlatformIO installs the async stack from `platformio.ini`, and the repository includes a local OneWire copy for PlatformIO/CLI builds.

<details>
<summary><strong>Verified PlatformIO toolchain (esp32-s3-n16r8)</strong> — click to expand</summary>

Resolved via `platformio pkg list --environment esp32-s3-n16r8`:

- Platform: `espressif32 @ 7.0.1`
- `framework-arduinoespressif32 @ 3.20017.241212+sha.dcc1105b`
- `framework-espidf @ 4.60001.0`
- `tool-cmake @ 3.30.2`
- `tool-esp-rom-elfs @ 0.0.1+20241011`
- `tool-esptoolpy @ 2.41100.0`
- `tool-idf @ 1.0.1`
- `tool-mconf @ 1.4060000.20190628`
- `tool-ninja @ 1.9.0`
- `toolchain-esp32ulp @ 1.23800.240113`
- `toolchain-riscv32-esp @ 8.4.0+2021r2-patch5`
- `toolchain-xtensa-esp-elf @ 15.2.0+20251204`
- `toolchain-xtensa-esp32s3 @ 8.4.0+2021r2-patch5`

If PlatformIO resolves a different platform/toolchain version and you hit a build issue, pin `platform = espressif32@7.0.1` in `platformio.ini` to match this known-good set.

</details>

### 3. Configure fallback credentials

1. Locate `example.secrets.h` in the root directory.
2. Copy and rename it to `secrets.h`.
3. Populate it with baseline defaults. These are only used on the first boot, because saved Web UI settings override them in NVS.

### 4. Build and flash

Use the Arduino IDE, Arduino CLI, or PlatformIO path that matches your setup, then upload the sketch and LittleFS data folder.

## 🚀 Setup & Installation

### 1. Upload the Web UI (LittleFS) - _CRITICAL STEP_

The C++ code alone will not serve the web interface. Upload the `data/` folder, which contains the offline UI assets, to the ESP32's flash memory.

- Press `Ctrl+Shift+P` (or `Cmd+Shift+P`), type "Upload LittleFS to Pico/ESP8266/ESP32", and execute.

### Arduino CLI example

If you prefer command-line builds, use the same ESP32-S3 board options shown above. Add the repo's `lib/` folder to the library search path so the checked-in OneWire copy is found during the build.

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3:UploadSpeed=115200,USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CPUFreq=240,UploadMode=default --libraries .\lib .\HyGrow_IoT.ino
```

---

## 📡 WebSocket API Protocol

The Web Doctor UI communicates with the ESP32 entirely over a single WebSocket connection at `/ws`.

### Authentication (single-owner login)

Every WebSocket connection starts unauthenticated — including reconnects — and must complete a handshake before any other command is accepted. An unauthenticated connection that sends anything other than `auth` is silently dropped (no error frame), so it can't learn anything about command validity either.

**Connection sequence:**

1. On connect, the server immediately sends `{"type": "auth_status", "setup_required": true | false}` — `true` if no admin password has ever been set on this device, `false` if one already exists.
2. The client responds with `{"command": "auth", "password": "..."}` (first-time setup: this also sets the password) or `{"command": "auth", "token": "..."}` (a previously-issued session token, tried silently before ever showing a login screen).
3. The server replies with `{"type": "auth_result", "ok": true | false, "token": "..."}`. On success, `token` is a fresh session token the client should store (the frontend keeps it in `localStorage`) and replay as step 2 on future connections. On failure, the client falls back to a password prompt.
4. Once authenticated, the connection immediately receives a full snapshot (`config`, `vitals`, `data`) rather than waiting for the next broadcast tick, plus any backlogged terminal log lines.

**Single remembered session:** the device holds exactly one valid session token at a time (`s_sessionToken` in `task_network.cpp`), not one per device. Logging in from a second browser/device overwrites that token — the first device's *live* connection keeps working until it reloads or reconnects, at which point its stored token is no longer recognized and it's sent back to the login screen. This is intentional single-owner behavior, not a bug, but it's worth knowing if you're wondering why a previously-logged-in tab suddenly asks for the password again.

- `{"command": "change_password", "current": "...", "new_pass": "..."}` — requires the current password even though the connection is already authenticated, so a stolen/left-open session token alone can't lock the real owner out. Response: `{"type": "change_password_result", "ok": true | false, "token": "...", "error": "..."}`. On success, the response's `token` re-authenticates this same connection against the fresh password (every other session's token, including this one's old value, is invalidated).

**BOOT-button recovery:** holding the onboard BOOT button resets the admin password/session so a lost password doesn't permanently lock a device out (see `state.cpp` / `led_status.cpp` for the hold-duration thresholds).

### Command acknowledgements

Every state-changing command below (everything except `request_vitals`, and except `reset_sensor_pin`/`reboot`/`factory_reset`, which restart the device before a reply would matter) replies directly to the requesting client with:

```json
{"type": "command_result", "command": "save_wifi", "ok": true}
```

or, if rejected:

```json
{"type": "command_result", "command": "save_pins", "ok": false, "error": "GPIO 19 and 20 are reserved for USB..."}
```

The frontend's save buttons wait for this ack before showing "Saved!" — the socket being open is not the same as the device having actually applied the change, so nothing shows success until this frame confirms it.

### Commands (Client -> ESP32)

- `{"command": "save_wifi", "ssid": "...", "pass": "..."}`
- `{"command": "save_firebase", "proj": "...", "api": "...", "email": "...", "pass": "...", "col": "..."}`
- `{"command": "save_pins", "pin_tds": 2, "pin_dht": 6, "pin_ph": 7, "pin_wt": 4, "pin_wl": 1, "pin_sda": 8, "pin_scl": 9, "pin_wlp": 5}` _(any field can be omitted to leave that pin unchanged; `-1` disables that sensor; requires reboot to apply; rejected server-side if any pin is 19/20 or duplicates another enabled sensor's pin — see [Forbidden Pins](#️-forbidden-pins-gpio-19--gpio-20))_
- `{"command": "reset_sensor_pin", "sensor": "tds" | "dht" | "ph" | "wt" | "wl" | "light"}` _(resets that sensor's pin(s) to the compiled default, clears its auto-disable flag, and reboots automatically)_
- `{"command": "save_sensor_enabled", "sensor": "tds" | "dht" | "ph" | "wt" | "wl" | "light", "enabled": true}` _(the per-sensor counterpart to `save_pins` — flips `sensor_enabled[i]`; enabling also restores that sensor's pin(s) from `-1` if needed; requires reboot to apply)_
- `{"command": "save_features", "demo": false, "fb_en": true}` _(any field can be omitted to leave that flag unchanged; none of the two require a reboot)_
- `{"command": "save_intervals", "int_read": 2000, "int_ws": 1000, "int_vit": 1000, "int_fb": 10000}` _(all values in ms, clamped server-side to 2000–60000; any field can be omitted to leave that interval unchanged)_
- `{"command": "calibrate_tds", "tds_k": 1.05}`
- `{"command": "calibrate_ph", "offset": 0.1, "slope": 1.02}`
- `{"command": "reboot"}`
- `{"command": "factory_reset"}` _(Wipes NVS namespace and reboots into SoftAP mode)_
- `{"command": "request_vitals"}` _(asks the device to immediately push a vitals frame)_

### Vitals fields worth knowing about

Every `vitals` frame also includes `wifi_status` (`"connected"` | `"ap_mode"` — shown on the dashboard's Uplink Status tile so a user on the SoftAP fallback network can tell) and Firebase upload health (`firebase_ready`, `firebase_last_ok_ms`, `firebase_last_error` — shown under Cloud Provisioning's Save Credentials button) so a silently-failing Firestore upload doesn't go unnoticed until the mobile app's data goes stale.

---

## 🌈 LED Error Color Codes

`led_status.cpp` defines a color per sensor for `ledCycleErrors()` to cycle through on the onboard WS2812. `task_sensor.cpp`'s read loop calls it every cycle: the LED is off while every enabled sensor's last read succeeded, or cycles through the colors below every 500ms for whichever enabled sensors currently have a failed last read. The mapping:

- 🔴 **Red**: Water Level failure
- 🟡 **Yellow**: BH1750 Light failure
- 🟣 **Purple**: TDS failure
- 🟠 **Orange**: DHT22 failure
- 🔵 **Blue**: pH Sensor failure
- 🩵 **Cyan**: DS18B20 Water Temp failure
- ⚫ **Off**: System Healthy — every enabled sensor's last read succeeded
