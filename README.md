# HyGrow-IoT: Advanced ESP32-S3 Dual-Core Sensor Pipeline

![ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-blue)
![FreeRTOS](https://img.shields.io/badge/OS-FreeRTOS-orange)
![Firebase](https://img.shields.io/badge/Database-Firestore-yellow)
![Status](https://img.shields.io/badge/Status-Active-success)

A robust, professional-grade IoT firmware for the ESP32-S3. HyGrow-IoT reads real-time environmental and hydroponic data from 6 different sensors, processes it asynchronously, and transmits it to Firebase Firestore.

This project uses a **Dual-Core FreeRTOS Architecture** and a **LittleFS-hosted Web Application** to provide a 100% offline-capable diagnostic dashboard, dynamic state configuration, and zero-blocking hardware reads.

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
| ---------------------------------------------------------------------------------------------------------------------------------- | -------- | ----------------------------- | ------------------------------------------------------------------ |
| **[Water Level Sensor](https://amzn.in/d/0cKf4nuQ)**                                                                               | Analog   | GPIO 1 (Sig) / GPIO 5 (Pwr)   | Powered dynamically to drastically reduce electrolytic corrosion.  |
| **[BH1750 Light Sensor](https://amzn.in/d/09NZHxCq)**                                                                              | I2C      | GPIO 21 (SDA) / GPIO 22 (SCL) | Digital ambient light detection.                                   |
| **[DFRobot Gravity Analog TDS](https://robocraze.com/products/dfrobot-gravity-analog-tds-water-quality-sensor-meter-for-arduino)** | Analog   | GPIO 14                       | Uses median filtering in code for noise reduction.                 |
| **[Hexonix DHT22 AM2302](https://amzn.in/d/07a1dbpF)**                                                                             | Digital  | GPIO 4                        | Temperature & Humidity. Module includes built-in pull-up resistor. |
| **[DFRobot Gravity Lab pH V2](https://robu.in/product/dfrobot-gravity-lab-grade-analog-ph-sensor-meter-kit-v2/)**                  | Analog   | GPIO 32                       | Lab-grade analog pH sensing. Native 3.3V support!                  |
| **[amiciSense DS18B20 Kit](https://amzn.in/d/0exQsfGD)**                                                                           | OneWire  | GPIO 15                       | Waterproof temp probe. Kit includes the terminal adapter.          |
| **Built-in RGB LED**                                                                                                               | NeoPixel | GPIO 48                       | Onboard WS2812 used for system health visualization.               |

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
- `board = esp32-s3-devkitc-1`
- `build_flags` enable USB CDC on boot and suppress PlatformIO warnings
- `board_build.flash_mode = qio`
- `board_build.f_cpu = 240000000L`
- `board_upload.flash_size = 16MB`
- `lib_deps` uses `ESP32Async/AsyncTCP` and `ESP32Async/ESPAsyncWebServer` for the async web stack

If you want an Arduino IDE build to match exactly, keep the board profile and the settings above aligned with the README section.

---

## 🚀 Setup & Installation

### 1. Library Dependencies

Install the following libraries via the Arduino Library Manager:

- `FirebaseClient` by Mobizt (v2.2.13)
- `ESP Async WebServer` by ESP32Async (v3.11.2)
- `Async TCP` by ESP32Async
- `ArduinoJson` by Benoit Blanchon (v7.4.3)
- `Adafruit NeoPixel` by Adafruit
- `BH1750` by Christopher Laws
- `DHT sensor library` by Adafruit
- `DallasTemperature` by Miles Burton

### 2. Configure Fallback Credentials

1. Locate `example.secrets.h` in the root directory.
2. Copy and rename to `secrets.h`.
3. Populate it with baseline defaults. _Note: These are only used on the very first boot. Once you save settings via the Web UI, the NVS memory permanently overrides `secrets.h`._

### 3. Flash the Firmware

Choose one of the supported build paths:

- **Arduino IDE:** open the sketch in the IDE, select the ESP32-S3 board profile from the README, and click **Upload**.
- **PlatformIO (VS Code):** the project now includes a [platformio.ini](platformio.ini) file, so you can open the folder in VS Code with the PlatformIO extension and build/upload from the PlatformIO toolbar.

### 4. Upload the Web UI (LittleFS) - _CRITICAL STEP_

The C++ code alone will not serve the web interface. You must upload the `data/` folder (which contains the compiled offline Tailwind CSS and Modular JS) to the ESP32's flash memory.

- Press `Ctrl+Shift+P` (or `Cmd+Shift+P`), type "Upload LittleFS to Pico/ESP8266/ESP32", and execute.

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
