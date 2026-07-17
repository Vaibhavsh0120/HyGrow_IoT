# HyGrow-IoT: ESP32-S3 Hydroponics Monitor

![ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-blue)
![FreeRTOS](https://img.shields.io/badge/OS-FreeRTOS-orange)
![Firebase](https://img.shields.io/badge/Database-Firestore-yellow)
![Status](https://img.shields.io/badge/Status-Active-success)

A compact ESP32-S3 firmware for monitoring hydroponic and environmental data from six sensors. It serves a local dashboard from LittleFS, supports native OTA updates, and can be built with either Arduino IDE, Arduino CLI, or PlatformIO.

---

## 🌟 Key Features & New Architecture

- **Dual-Core Processing (FreeRTOS):**
  - **Core 0:** Handles WiFi, the LittleFS Web Server, WebSockets, NVS storage, and asynchronous Firebase uploads.
  - **Core 1:** Exclusively dedicated to precise hardware timing, sensor reads, and Vapor Pressure Deficit (VPD) calculations without network-induced latency.
- **Offline-First "Stitch" UI:** A beautiful, responsive "liquid-glass" Single Page Application hosted directly on the ESP32's flash memory. No internet or external CDNs required.
- **True Offline Fallback:** If the configured WiFi fails, the ESP32 automatically broadcasts a `HyGrow-Setup` Access Point (SoftAP) for local configuration and diagnostics.
- **Dynamic NVS Configuration (Web Doctor):** Update WiFi credentials, Firebase keys, sensor GPIO pins, and interval timings directly from the web dashboard. Settings persist across reboots via Non-Volatile Storage (NVS).
- **Live Sensor Calibration:** Calibrate pH (slope/offset) and TDS (K-factor) live from the browser without recompiling code.
- **Over-The-Air (OTA) Updates:** Flash new `.bin` firmware files wirelessly via the built-in OTA portal.
- **Smart RGB Error Feedback:** Uses the ESP32-S3's onboard WS2812 NeoPixel to display system health and cycle through specific color codes for disconnected/failed hardware.

---

## 🛠 Hardware & Wiring

**Supported Board:** ESP32-S3 N16R8 DevKit

This project is optimized for modular sensor kits that include built-in resistors and signal conditioning (such as terminal adapters and pull-ups). This allows for **direct connection** to the ESP32-S3 without requiring additional breadboard circuitry.

### Specific Sensor Hardware Used

| Sensor Module & Purchase Link                                                                                                      | Protocol | ESP32-S3 Pin (Default)\*      | Notes                                                              |
| ---------------------------------------------------------------------------------------------------------------------------------- | -------- | ------------------------------ | ------------------------------------------------------------------ |
| **[Water Level Sensor](https://amzn.in/d/0cKf4nuQ)**                                                                               | Analog   | GPIO 1 (Sig) / GPIO 5 (Pwr)    | Powered dynamically to drastically reduce electrolytic corrosion. |
| **[BH1750 Light Sensor](https://amzn.in/d/09NZHxCq)**                                                                              | I2C      | GPIO 21 (SDA) / GPIO 22 (SCL)  | Digital ambient light detection.                                   |
| **[DFRobot Gravity Analog TDS](https://robocraze.com/products/dfrobot-gravity-analog-tds-water-quality-sensor-meter-for-arduino)** | Analog   | GPIO 14                        | Uses median filtering in code for noise reduction.                 |
| **[Hexonix DHT22 AM2302](https://amzn.in/d/07a1dbpF)**                                                                             | Digital  | GPIO 4                         | Temperature & Humidity. Module includes built-in pull-up resistor.|
| **[DFRobot Gravity Lab pH V2](https://robu.in/product/dfrobot-gravity-lab-grade-analog-ph-sensor-meter-kit-v2/)**                  | Analog   | GPIO 32                        | Lab-grade analog pH sensing. Native 3.3V support!                  |
| **[amiciSense DS18B20 Kit](https://amzn.in/d/0exQsfGD)**                                                                           | OneWire  | GPIO 15                        | Waterproof temp probe. Kit includes the terminal adapter.          |
| **Built-in RGB LED**                                                                                                               | NeoPixel | GPIO 48                        | Onboard WS2812 used for system health visualization.               |

_\*Note: GPIO pins can now be dynamically reassigned in the Web UI Settings tab._

> **⚡ Power Note:** Thanks to the V2 specifications of the DFRobot modules, **every single sensor in this project shares a unified 3.3V and GND rail**. No 5V logic-level shifting is required!

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

### Commands (Client -> ESP32)

- `{"command": "save_wifi", "ssid": "...", "pass": "..."}`
- `{"command": "save_firebase", "proj": "...", "api": "...", "email": "...", "col": "..."}`
- `{"command": "calibrate_tds", "tds_k": 1.05}`
- `{"command": "calibrate_ph", "offset": 0.1, "slope": 1.02}`
- `{"command": "reboot"}`
- `{"command": "factory_reset"}` _(Wipes NVS namespace and reboots into SoftAP mode)_

---

## 🌈 LED Error Color Codes

If one or more sensors disconnect or fail, the LED cycles through specific diagnostic colors:

- 🔴 **Red**: Water Level failure
- 🟡 **Yellow**: BH1750 Light failure
- 🟣 **Purple**: TDS failure
- 🟠 **Orange**: DHT22 failure
- 🔵 **Blue**: pH Sensor failure
- 🩵 **Cyan**: DS18B20 Water Temp failure
- 🟢 **Green / Blue / Red (Solid)**: System Healthy (Water/Air Temp: Normal / Cold / Hot)
