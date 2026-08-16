# HyGrow-IoT: ESP32-S3 Hydroponics Monitor

![ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-blue)
![FreeRTOS](https://img.shields.io/badge/OS-FreeRTOS-orange)
![Firebase](https://img.shields.io/badge/Database-Firestore-yellow)
![Status](https://img.shields.io/badge/Status-Active-success)

A compact ESP32-S3 firmware for monitoring hydroponic and environmental
data from six sensors. It serves a local dashboard from LittleFS, syncs
live readings to the cloud so a companion app can show device status from
anywhere, and can be built with the Arduino IDE, Arduino CLI, or
PlatformIO.

> **Looking for technical/protocol-level documentation?** This README
> covers setup and day-to-day use. Implementation details (Firestore data
> model, WebSocket protocol, firmware internals) live in
> [`docs/`](docs/README.md).

---

## 🏛️ System Architecture Context

HyGrow is built on a strictly decoupled, 3-pillar architecture. This
codebase represents **Pillar 3** only.

- **Pillar 1: The React Native App & Hugging Face AI (The Brain)** —
  handles historical data, push notifications, and predictive plant-health
  models.
- **Pillar 2: Firebase / Firestore (The Bridge)** — a real-time mirror
  holding each device's current, live state (see [Cloud Sync](#-cloud-sync--device-status) below).
- **Pillar 3: The ESP32-S3 IoT Device (The Edge / This Codebase)** —
  a fault-tolerant "dumb pipe" and local configuration appliance. No
  historical data storage, no predictive logic on-device. It reads
  sensors, pushes state to Firestore, and hosts a local Web UI for
  setup, provisioning, and calibration.

---

## 🌟 Key Features

- **Dual-Core Processing (FreeRTOS):** sensor timing and networking run on
  separate cores so a slow network call never delays a sensor read.
- **Offline-First "Stitch" UI:** a responsive dashboard hosted entirely on
  the ESP32's own flash — no internet or external CDNs required to use it.
- **True Offline Fallback:** if configured WiFi fails, the device
  broadcasts a `HyGrow-Setup` access point for local configuration.
- **Dynamic Configuration (Web Doctor):** update WiFi, Firebase
  credentials, sensor pins, feature flags, and timing directly from the
  dashboard — all persisted across reboots.
- **Cloud Device Status:** each device keeps a live "Online"/"Offline"
  status in the cloud, detected automatically within roughly a minute of
  losing connectivity — see [Cloud Sync](#-cloud-sync--device-status).
- **Feature Flags:** Demo Mode (simulate all six sensors with no hardware
  wired up) and a Firebase Upload master switch, live-toggleable with no
  reboot.
- **Guided Sensor Calibration:** pH via a 2-point wizard, TDS via a
  1-point target wizard, both calculated in the browser.
- **Startup Validation & Auto-Disable:** every enabled sensor gets 5
  boot-time read attempts; one that fails all 5 is auto-disabled and can
  be re-enabled with one click.
- **Light / Dark / Auto Theme**, switchable and persisted per browser.
- **RGB Status LED:** at-a-glance sensor health — see
  [LED Status Colors](#-led-status-colors).

---

## 🛠 Hardware & Wiring

**Supported Board:** ESP32-S3 N16R8 DevKit

This project targets modular sensor kits with built-in resistors/signal
conditioning, so every sensor connects directly to the ESP32-S3 without
extra breadboard circuitry.

| Sensor Module & Purchase Link | Protocol | ESP32-S3 Pin (Default)\* | Notes |
| --- | --- | --- | --- |
| **[Water Level Sensor](https://amzn.in/d/0cKf4nuQ)** | Analog | GPIO 1 (Sig) / GPIO 5 (Pwr) | Power-gated: only energized for ~10ms per read to reduce electrolytic corrosion. |
| **[BH1750 Light Sensor](https://amzn.in/d/09NZHxCq)** | I2C | GPIO 8 (SDA) / GPIO 9 (SCL) | Digital ambient light detection. |
| **[DFRobot Gravity Analog TDS](https://robocraze.com/products/dfrobot-gravity-analog-tds-water-quality-sensor-meter-for-arduino)** | Analog | GPIO 2 | Uses median filtering in code for noise reduction. |
| **[Hexonix DHT22 AM2302](https://amzn.in/d/07a1dbpF)** | Digital | GPIO 6 | Temperature & Humidity. |
| **[DFRobot Gravity Lab pH V2](https://robu.in/product/dfrobot-gravity-lab-grade-analog-ph-sensor-meter-kit-v2/)** | Analog | GPIO 7 | Lab-grade analog pH sensing. Native 3.3V support. |
| **[amiciSense DS18B20 Kit](https://amzn.in/d/0exQsfGD)** | OneWire | GPIO 4 | Waterproof temp probe. |
| **Built-in RGB LED** | NeoPixel | GPIO 48 | Onboard WS2812 for system health visualization. |

_\*GPIO pins can be reassigned in the Web UI's Settings tab. The table
above lists the compiled fallback defaults from `config.h`, used on first
boot or after a factory reset._

> **⚡ Power Note:** every sensor in this project shares a unified 3.3V and
> GND rail — no 5V logic-level shifting required.

> **🔌 Default Enabled State:** every sensor ships **enabled** except
> **pH**, which ships **disabled** until you've calibrated it in real
> liquid (Settings, or the pH sensor's detail page → **Enable Power**).

### ⚠️ Forbidden Pins: GPIO 19 & GPIO 20

**Never assign any sensor, LED, or other peripheral to GPIO 19 or GPIO
20.** On the ESP32-S3, these are the native USB D-/D+ lines this firmware's
Serial connection depends on — assigning them elsewhere causes the board to
appear to randomly disconnect from your computer. The firmware validates
this both in the browser and on the device itself, and will refuse to save
a conflicting pin assignment. See
[`docs/FIRMWARE_ARCHITECTURE.md`](docs/FIRMWARE_ARCHITECTURE.md#2-forbidden-pins-gpio-19--20--three-defense-layers)
for the full defense-in-depth detail.

---

## 🚀 Getting Started

### 1. Pick a build path

- **Arduino IDE:** open [`HyGrow_IoT.ino`](HyGrow_IoT.ino) and build with
  the board settings below.
- **Arduino CLI:** install `arduino-cli`, then compile with the same
  ESP32-S3 options shown below.
- **PlatformIO:** open the folder in VS Code and use the environment named
  `esp32-s3-n16r8`.

### 2. Arduino IDE Board Settings (ESP32-S3 N16R8)

- **Board:** ESP32S3 Dev Module
- **USB CDC On Boot:** Enabled
- **CPU Frequency:** 240MHz (WiFi)
- **Flash Mode:** QIO 80MHz
- **Flash Size:** 16MB (128Mb)
- **Partition Scheme:** 16M Flash (3MB APP/9.9MB FATFS)
- **PSRAM:** OPI PSRAM
- **Upload Mode:** UART0 / Hardware CDC

PlatformIO users: [`platformio.ini`](platformio.ini) already matches this
board profile — just build the `esp32-s3-n16r8` environment. See
[`docs/FIRMWARE_ARCHITECTURE.md`](docs/FIRMWARE_ARCHITECTURE.md) if a
build issue points you toward a toolchain-version mismatch.

### 3. Install Arduino libraries

If using the Arduino IDE or Arduino CLI, install these (PlatformIO
installs them automatically from `platformio.ini`):

| Library | Author | Verified Version |
| --- | --- | --- |
| ESPAsyncWebServer | ESP32Async | 3.11.2 |
| AsyncTCP | ESP32Async | 3.4.10 |
| ArduinoJson | Benoit Blanchon | 7.4.3 |
| Adafruit NeoPixel | Adafruit | 1.15.5 |
| Adafruit Unified Sensor | Adafruit | 1.1.15 |
| DHT sensor library | Adafruit | 1.4.7 |
| DallasTemperature | Miles Burton | 3.11.0 |
| OneWire | Paul Stoffregen | 2.3.7 |
| BH1750 | Christopher Laws | 1.3.0 |

### 4. Create `secrets.h` — REQUIRED

The build will not compile without this file — there's no built-in
fallback for credentials.

1. Copy [`example.secrets.h`](example.secrets.h) to `secrets.h` in the
   project root (same folder as `HyGrow_IoT.ino`). It's already listed in
   `.gitignore`, so your real copy is never committed.
2. Fill in your WiFi SSID/password, a SoftAP recovery password (empty
   `""` for an open network, or 8+ characters), an admin password (or
   `""` to ship "Unconfigured"), and your Firebase credentials if you
   plan to use cloud sync — see [Cloud Sync](#-cloud-sync--device-status)
   below for how to get those.
3. These values are only used on first boot or after a Factory Reset —
   once you save settings via the Web UI, they live in NVS and
   `secrets.h`'s values are ignored.

### 5. Upload the Web UI (LittleFS) — critical step

The compiled firmware alone will not serve the local dashboard. Upload the
[`data/`](data/) folder to the ESP32's flash:

- **VS Code / PlatformIO:** `Ctrl+Shift+P` → "Upload LittleFS to
  Pico/ESP8266/ESP32".
- **Arduino IDE:** use the LittleFS upload plugin for your IDE version.

### 6. Build and flash

**Arduino CLI example:**

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3:UploadSpeed=115200,USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CPUFreq=240,UploadMode=default .\HyGrow_IoT.ino
```

Or use your IDE/PlatformIO's normal upload button.

### 7. First connect

On first boot with no saved WiFi credentials, the device starts its own
`HyGrow-Setup` access point. Connect to it, open the device's IP in a
browser, and use Settings to enter your real WiFi network. From then on it
connects to your network automatically on every boot.

---

## 🖥️ Using the Dashboard

Once connected, the dashboard (served locally by the device, no internet
required to view it) gives you:

- **Live sensor tiles** for all six sensors, with a health dot per sensor.
- **Settings** — WiFi, Firebase credentials, GPIO pin assignments, sensor
  enable/disable toggles, feature flags (Demo Mode, Firebase Upload), and
  timing intervals.
- **Live Calibration** — guided pH (2-point) and TDS (1-point) wizards,
  calculated in the browser.
- **Terminal** — a live log of what the device is doing, including any
  crash/reboot reason from the previous boot.

**Forgot your admin password?** Hold the onboard BOOT button to reset the
admin password/session without wiping any other settings.

**Factory reset** wipes WiFi, Firebase credentials, calibration, pins, and
the admin password. The Settings page requires typing `RESET` (all caps)
to confirm — there's no accidental single-click wipe.

**A sensor auto-disabled itself?** If a sensor fails all 5 of its
boot-time read attempts, it's automatically turned off so it doesn't spam
errors — fix the wiring, then click **Reset** on that sensor's card in
Settings (restores its default pin and re-enables it) or flip its
**Enabled** toggle back on.

---

## ☁️ Cloud Sync & Device Status

If Firebase Upload is enabled (Settings → Feature Flags) and credentials
are configured, the device keeps one document per physical device in
Cloud Firestore, mirroring its current state for the companion mobile app
(or anything else reading Firestore) to display.

### What gets stored

Each device writes to `devices/{your-device-id}` — its `device_id` is set
in `secrets.h` or Settings, so multiple HyGrow devices (`hygrow_001`,
`hygrow_002`, ...) can share one Firebase project without colliding. Each
document holds:

- The device's **status** — `"Online"` or `"Offline"` (see below).
- When it was **last updated** — a cloud-side timestamp, not the device's
  own clock (which has no battery-backed RTC and can't be trusted).
- The **current reading** from each of the 8 sensor/measurement values, or
  **`null`** for any that are currently disabled, mid-failure, or haven't
  produced a reading yet this boot. A `null` value means "don't trust this
  field right now" — it's never silently left at its last real number, and
  it's never a fake `0` (since `0` can be a genuine reading, e.g. an empty
  water tank).
- Its firmware version and uptime, for diagnostics.

### Online vs. Offline

**This distinction is the most important thing to understand about this
feature: status describes connectivity, not sensor health.**

- The device sets itself **Online** the instant any update successfully
  reaches Firestore — that alone proves it currently has both WiFi and a
  working path to the cloud.
- A single failed or disabled sensor does **not** affect this — a device
  with a broken pH probe but working WiFi still shows **Online**, with
  just `ph_val` sent as `null`.
- **Offline is never decided by the device itself** — a device that has
  actually lost power or WiFi has no way to report that itself. Instead, a
  small cloud backend function checks every device's last-updated time
  roughly once a minute, and flips any device to **Offline** once its last
  update is more than **30 seconds** old. In practice this means a device
  going dark is marked Offline somewhere between about 30 and 90 seconds
  later, not instantly — a real, documented limitation of building this on
  a serverless scheduled check rather than an always-on server. Full
  reasoning: [`docs/FIRESTORE_ARCHITECTURE.md`](docs/FIRESTORE_ARCHITECTURE.md#43-the-30-second-threshold-vs-cloud-schedulers-real-floor--an-honest-limitation).
- When the device reconnects, its very next successful update flips it
  straight back to **Online** with a fresh timestamp.

### Security

- **Reading** device state is open to any client with your project's
  Firebase config — matching how the companion app needs to display it.
- **Writing** requires valid Firebase sign-in credentials (the same ones
  you put in Settings), and even then a device can only write a
  well-formed update to its *own* document — it can never set itself
  `"Offline"` or fake the update timestamp; both of those are backend/
  server-controlled.
- Firestore rules reject any write that doesn't look like a genuine
  device update — see
  [`docs/FIRESTORE_ARCHITECTURE.md`](docs/FIRESTORE_ARCHITECTURE.md#5-security-model)
  for the full rule set and its one honestly-documented limitation (a
  compromised device credential could still write to *another* device's
  document within the same Firebase project).

### Setting it up

1. In the [Firebase Console](https://console.firebase.google.com/),
   create a project and enable **Firestore** (Native mode).
2. Under **Authentication → Sign-in method**, enable **Email/Password**,
   then add a user matching what you'll put in `secrets.h` (or Settings →
   Cloud Provisioning).
3. Deploy the security rules and the Offline-detection backend function
   from this repo's `firebase/` folder:
   ```bash
   cd firebase
   npm install -g firebase-tools   # if you don't already have it
   firebase login
   npm --prefix functions install
   # edit .firebaserc first — replace YOUR_FIREBASE_PROJECT_ID with your real project id
   firebase deploy --only firestore:rules,functions
   ```
   Cloud Functions and Cloud Scheduler both require Firebase's Blaze
   (pay-as-you-go) plan — the free-tier quota comfortably covers a small
   number of devices checked once a minute.
4. In the device's Settings → Cloud Provisioning, enter your Project ID,
   Web API Key, and the email/password from step 2, then **Save
   Credentials** and use **Test Connection** to confirm it's reachable.
5. Turn on **Firebase Upload** (Settings → Feature Flags) if it isn't
   already. Watch `devices/{your device_id}` appear in the Firestore
   Console within one upload cycle (10 seconds by default).

Full technical design — exact field list, why a genuine server timestamp
requires a specific Firestore API call, and the complete security-rule
writeup — lives in
[`docs/FIRESTORE_ARCHITECTURE.md`](docs/FIRESTORE_ARCHITECTURE.md).

---

## 🌈 LED Status Colors

The onboard WS2812 LED gives an at-a-glance read on sensor health:

| Signal | Meaning |
| --- | --- |
| ⚫ **Off** | System healthy — every enabled sensor's last read succeeded |
| 🔴 **Red** (solid) | Water Level sensor failing |
| 🟡 **Yellow** (solid) | BH1750 Light sensor failing |
| 🟣 **Purple** (solid) | TDS sensor failing |
| 🟠 **Orange** (solid) | DHT22 (Temp/Humidity) sensor failing |
| 🔵 **Blue** (solid) | pH sensor failing |
| 🩵 **Cyan** (solid) | DS18B20 Water Temp sensor failing |
| ⚪ **White** (fast strobe) | 2 or more sensors failing at once |
| 🟣 **Magenta** (solid, boot-time only) | LittleFS filesystem mount failed — re-flash the filesystem image and reset the board |

Disabled sensors are never counted here, no matter their last error.
Implementation detail: [`docs/FIRMWARE_ARCHITECTURE.md`](docs/FIRMWARE_ARCHITECTURE.md#8-led-error-color-codes--implementation-detail).

---

## 📚 Further Reading (Developers & AI Assistants)

Deeper technical documentation lives in [`docs/`](docs/README.md):

- [`docs/FIRESTORE_ARCHITECTURE.md`](docs/FIRESTORE_ARCHITECTURE.md) — the
  full Firestore data model, Online/Offline detection logic, security
  rules, and Cloud Functions backend.
- [`docs/WEBSOCKET_API.md`](docs/WEBSOCKET_API.md) — the local `/ws`
  protocol: every command, payload, and broadcast frame.
- [`docs/FIRMWARE_ARCHITECTURE.md`](docs/FIRMWARE_ARCHITECTURE.md) —
  firmware internals: dual-core split, pin safety, calibration bounds,
  save/crash reliability, and sensor auto-disable.
