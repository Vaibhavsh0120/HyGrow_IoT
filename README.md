# HyGrow-IoT

**ESP32-S3 firmware for monitoring a hydroponic system — six sensors, a
local web dashboard, and optional cloud sync.**

![Platform](https://img.shields.io/badge/Platform-ESP32--S3-blue)
![Framework](https://img.shields.io/badge/Framework-Arduino%20%2F%20FreeRTOS-orange)
![Status](https://img.shields.io/badge/Status-Active-success)

HyGrow reads water level, light, TDS, temperature/humidity, pH, and water
temperature, then serves a live dashboard directly from the board's own
flash — no app, no internet connection required to use it. Wi-Fi and
Firebase credentials, sensor pins, and calibration are all configured
through the same dashboard, and everything persists across reboots.

---

## Features

- **Six sensors, one dashboard** — live readings, per-sensor health
  status, and guided calibration (pH via 2-point, TDS via 1-point wizard),
  all in the browser.
- **Works with no internet** — the dashboard is hosted on the ESP32
  itself. If it can't join your Wi-Fi, it opens its own `HyGrow-Setup`
  network so you can configure it from a phone or laptop.
- **Dual-core by design** — sensor reading and networking run on separate
  FreeRTOS cores, so a slow network call never delays a sensor read.
- **Demo Mode** — try the full dashboard with simulated sensor data before
  you've wired anything up.
- **Auto-disable on failure** — a sensor that fails its boot-time read
  attempts is turned off automatically instead of spamming errors; fix the
  wiring and re-enable it with one click.
- **Optional cloud sync** — mirror each device's current state to Cloud
  Firestore so it can be read from anywhere (e.g. a companion app).
- **Status LED** — an onboard RGB LED shows sensor health at a glance, no
  screen required.
- **Password-protected settings**, with a physical BOOT-button recovery
  path if you forget it.

---

## Hardware & Wiring

**Board:** ESP32-S3 N16R8 DevKit (16MB flash / 8MB PSRAM)

Every sensor below uses a breakout module with built-in signal
conditioning, so it wires straight to the ESP32-S3 — no extra resistors or
breadboard circuitry needed. All sensors share a common 3.3V/GND rail.

| Sensor Module & Purchase Link | Protocol | Default Pin | Notes |
| --- | --- | --- | --- |
| **[Water Level Sensor](https://amzn.in/d/0cKf4nuQ)** | Analog | GPIO 1 (signal) / GPIO 5 (power) | Power-gated — only energized briefly per reading to reduce corrosion. |
| **[BH1750 Light Sensor](https://amzn.in/d/09NZHxCq)** | I2C | GPIO 8 (SDA) / GPIO 9 (SCL) | Digital ambient light detection. |
| **[DFRobot Gravity Analog TDS](https://robocraze.com/products/dfrobot-gravity-analog-tds-water-quality-sensor-meter-for-arduino)** | Analog | GPIO 2 | Median-filtered in software. |
| **[Hexonix DHT22 AM2302](https://amzn.in/d/07a1dbpF)** | Digital | GPIO 6 | Temperature & humidity. |
| **[DFRobot Gravity Lab pH V2](https://robu.in/product/dfrobot-gravity-lab-grade-analog-ph-sensor-meter-kit-v2/)** | Analog | GPIO 7 | Ships **disabled** until calibrated — see [Using the Dashboard](#using-the-dashboard). Native 3.3V support. |
| **[amiciSense DS18B20 Kit](https://amzn.in/d/0exQsfGD)** | OneWire | GPIO 4 | Waterproof water-temperature probe. |
| Built-in RGB LED | NeoPixel | GPIO 48 | Onboard WS2812 for system health visualization. |

Pins can be reassigned from the dashboard's Settings tab; the table above
is just the compiled-in default from `config.h`.

> **Never assign anything to GPIO 19 or 20.** They're the ESP32-S3's
> native USB D-/D+ lines — using them for a sensor makes the board look
> like it's randomly disconnecting from your computer. The firmware
> refuses to save a pin assignment on either one.

---

## Getting Started

### 1. Pick a build tool

- **PlatformIO** (recommended) — open the folder in VS Code;
  [`platformio.ini`](platformio.ini) is already configured for this board.
  Build/upload the `esp32-s3-n16r8` environment.
- **Arduino IDE** — open [`HyGrow_IoT.ino`](HyGrow_IoT.ino) and use the
  board settings below.
- **Arduino CLI** — same board settings, see the example command in
  [step 5](#5-build-and-flash).

**Arduino IDE board settings (ESP32S3 Dev Module):**

| Setting | Value |
| --- | --- |
| USB CDC On Boot | Enabled |
| CPU Frequency | 240MHz (WiFi) |
| Flash Mode | QIO 80MHz |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| PSRAM | OPI PSRAM |
| Upload Mode | UART0 / Hardware CDC |

### 2. Install libraries (Arduino IDE / CLI only)

PlatformIO installs these automatically. For Arduino IDE/CLI, install via
Library Manager:

| Library | Author | Version |
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

### 3. Create `secrets.h` — required

The firmware will not compile without this file.

1. Copy [`example.secrets.h`](example.secrets.h) to a new file named
   `secrets.h` in the project root. It's already in `.gitignore`, so your
   real copy is never committed.
2. Fill in your Wi-Fi SSID/password, a SoftAP recovery password (empty
   `""` for open, or 8+ characters), an admin password (or `""` to ship
   "Unconfigured"), and your Firebase credentials if you want
   [cloud sync](#cloud-sync-optional).
3. These values are only used on first boot or after a factory reset —
   once you save settings from the dashboard, they're stored on-device
   and `secrets.h` is no longer read.

### 4. Upload the web dashboard (LittleFS)

The compiled firmware alone won't serve the dashboard — you also need to
upload the [`data/`](data/) folder to the board's flash:

- **PlatformIO:** `Ctrl+Shift+P` → "Upload Filesystem Image".
- **Arduino IDE:** use the LittleFS upload plugin for your IDE version.

### 5. Build and flash

Use your IDE's or PlatformIO's normal upload button, or with Arduino CLI:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3:UploadSpeed=115200,USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CPUFreq=240,UploadMode=default .\HyGrow_IoT.ino
```

### 6. First connect

With no saved Wi-Fi credentials, the device starts its own `HyGrow-Setup`
Wi-Fi network. Connect to it, open the device's IP in a browser, and use
Settings to enter your real network. From then on it connects
automatically on every boot.

---

## Using the Dashboard

Once connected, the dashboard (served locally, no internet needed to view
it) gives you:

- **Live sensor tiles** for all six sensors, with a health indicator each.
- **Settings** — Wi-Fi, Firebase credentials, pin assignments, sensor
  enable/disable, feature flags, and timing intervals.
- **Live Calibration** — guided pH (2-point) and TDS (1-point) wizards.
- **Terminal** — a live device log, including the reason for the last
  reboot.

**Forgot your admin password?** Hold the onboard BOOT button for 10
seconds to reset it without touching any other settings.

**Factory reset** wipes Wi-Fi, Firebase credentials, calibration, pins,
and the admin password. Hold BOOT for 20 seconds, or type `RESET` on the
Settings page — there's no accidental one-click wipe.

**A sensor turned itself off?** If a sensor fails all 5 of its boot-time
read attempts, it's auto-disabled so it doesn't spam errors. Fix the
wiring, then hit **Reset** on that sensor's card in Settings (restores its
default pin and re-enables it) or flip its **Enabled** toggle back on.

---

## Cloud Sync (optional)

If you enable Firebase Upload (Settings → Feature Flags) and provide
credentials, the device keeps one Firestore document per physical device,
mirroring its current sensor readings and connection status.

**What this does and doesn't cover:** the device only ever marks itself
`"Online"`, the instant an update reaches Firestore. It never marks
itself `"Offline"` — a device that's actually lost power or Wi-Fi has no
way to say so about itself. If you want automatic offline detection,
you'll need to build something that watches each document's
`lastUpdated` timestamp and flags it stale after a threshold you choose
(e.g. a small scheduled Cloud Function) — that piece isn't included in
this repo.

A single failed sensor never affects the `Online`/`Offline` status —
that field means "device is reachable," not "every sensor works." A
disabled or failing sensor just sends `null` for that one reading.

### Setting it up

1. In the [Firebase Console](https://console.firebase.google.com/),
   create a project and enable **Firestore** (Native mode).
2. Under **Authentication → Sign-in method**, enable **Email/Password**
   and add a user matching what you'll put in `secrets.h` (or Settings →
   Cloud Provisioning).
3. Write Firestore security rules that let this user read/write only its
   own device document — the default "test mode" rules are open to
   anyone and shouldn't be used past initial testing.
4. On the device, go to Settings → Cloud Provisioning and enter your
   Project ID, Web API Key, and the email/password from step 2. Save,
   then use **Test Connection** to confirm it's reachable.
5. Turn on **Firebase Upload** (Settings → Feature Flags). You should see
   `devices/{your device_id}` appear in the Firestore Console within one
   upload cycle (10 seconds by default).

---

## LED Status Colors

The onboard RGB LED gives an at-a-glance read on sensor health:

| Signal | Meaning |
| --- | --- |
| ⚫ Off | Every enabled sensor's last read succeeded |
| 🔴 Red (solid) | Water Level sensor failing |
| 🟡 Yellow (solid) | Light sensor failing |
| 🟣 Purple (solid) | TDS sensor failing |
| 🟠 Orange (solid) | DHT22 (temp/humidity) sensor failing |
| 🔵 Blue (solid) | pH sensor failing |
| 🩵 Cyan (solid) | Water temperature sensor failing |
| ⚪ White (fast strobe) | 2 or more sensors failing at once |
| 🟣 Magenta (solid, boot only) | Filesystem mount failed — re-flash the LittleFS image |

Disabled sensors are never counted here, regardless of their last error.

---

## Project Structure

```
HyGrow_IoT/
├── HyGrow_IoT.ino          # Entry point: boot sequence, FreeRTOS task setup
├── config.h                # Compile-time defaults (pins, timing, sensor IDs)
├── example.secrets.h       # Template — copy to secrets.h and fill in
├── platformio.ini          # PlatformIO board/library config
├── partitions.csv          # 16MB flash partition table (app + LittleFS)
├── src/
│   ├── core/                # Networking, web server, WebSocket, auth, Firestore sync
│   ├── sensors/              # One file per physical sensor
│   └── utils/                 # Status LED
└── data/                    # Web dashboard, flashed to LittleFS separately
    ├── index.html
    ├── css/ · js/ · fonts/ · icons/
    └── manifest.json        # PWA manifest
```

See [`PROGRESS.md`](PROGRESS.md) for current status, open issues, and
past design decisions.

---

## License

No license file yet — add one (MIT, Apache-2.0, etc.) before sharing this
repo publicly if you want to make the terms explicit.
