# HyGrow-IoT: Advanced ESP32-S3 Dual-Core Sensor Pipeline

![ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-blue)
![FreeRTOS](https://img.shields.io/badge/OS-FreeRTOS-orange)
![Firebase](https://img.shields.io/badge/Database-Firestore-yellow)
![Status](https://img.shields.io/badge/Status-Active-success)

A robust, professional-grade IoT firmware for the ESP32-S3. HyGrow-IoT reads real-time environmental and hydroponic data from 6 different sensors, processes it asynchronously, and transmits it to Firebase Firestore[cite: 7].

This project utilizes a **Dual-Core FreeRTOS Architecture** and a **LittleFS-hosted Web Application** to provide a 100% offline-capable diagnostic dashboard, dynamic state configuration, and zero-blocking hardware reads[cite: 7].

---

## 🌟 Key Features & New Architecture

- **Dual-Core Processing (FreeRTOS):**
  - **Core 0:** Handles WiFi, ElegantOTA, the LittleFS Web Server, WebSockets, NVS storage, and asynchronous Firebase uploads[cite: 7].
  - **Core 1:** Exclusively dedicated to precise hardware timing, sensor reads, and Vapor Pressure Deficit (VPD) calculations without network-induced latency[cite: 7].
- **Offline-First "Stitch" UI:** A beautiful, responsive "liquid-glass" Single Page Application hosted directly on the ESP32's flash memory. No internet or external CDNs required.
- **True Offline Fallback:** If the configured WiFi fails, the ESP32 automatically broadcasts a `HyGrow-Setup` Access Point (SoftAP) for local configuration and diagnostics.
- **Dynamic NVS Configuration (Web Doctor):** No more hardcoding! Update WiFi credentials, Firebase keys, sensor GPIO pins, and interval timings directly from the web dashboard. Settings persist across reboots via Non-Volatile Storage (NVS).
- **Live Sensor Calibration:** Calibrate pH (slope/offset) and TDS (K-factor) live from the browser without recompiling code.
- **Over-The-Air (OTA) Updates:** Flash new `.bin` firmware files wirelessly via the integrated ElegantOTA portal.
- **Smart RGB Error Feedback:** Uses the ESP32-S3's onboard WS2812 NeoPixel to display system health and cycle through specific color codes for disconnected/failed hardware[cite: 7].

---

## 🛠 Hardware & Wiring

**Supported Board:** ESP32-S3 N16R8 DevKit[cite: 7]

This project is optimized for modular sensor kits that include built-in resistors and signal conditioning (such as terminal adapters and pull-ups). This allows for **direct connection** to the ESP32-S3 without requiring additional breadboard circuitry[cite: 7].

### Specific Sensor Hardware Used

| Sensor Module & Purchase Link                                                                                                      | Protocol | ESP32-S3 Pin (Default)\*      | Notes                                                                       |
| ---------------------------------------------------------------------------------------------------------------------------------- | -------- | ----------------------------- | --------------------------------------------------------------------------- |
| **[Water Level Sensor](https://amzn.in/d/0cKf4nuQ)**                                                                               | Analog   | GPIO 1 (Sig) / GPIO 5 (Pwr)   | Powered dynamically to drastically reduce electrolytic corrosion[cite: 7].  |
| **[BH1750 Light Sensor](https://amzn.in/d/09NZHxCq)**                                                                              | I2C      | GPIO 21 (SDA) / GPIO 22 (SCL) | Digital ambient light detection[cite: 7].                                   |
| **[DFRobot Gravity Analog TDS](https://robocraze.com/products/dfrobot-gravity-analog-tds-water-quality-sensor-meter-for-arduino)** | Analog   | GPIO 14                       | Uses median filtering in code for noise reduction[cite: 7].                 |
| **[Hexonix DHT22 AM2302](https://amzn.in/d/07a1dbpF)**                                                                             | Digital  | GPIO 4                        | Temperature & Humidity. Module includes built-in pull-up resistor[cite: 7]. |
| **[DFRobot Gravity Lab pH V2](https://robu.in/product/dfrobot-gravity-lab-grade-analog-ph-sensor-meter-kit-v2/)**                  | Analog   | GPIO 32                       | Lab-grade analog pH sensing. Native 3.3V support![cite: 7]                  |
| **[amiciSense DS18B20 Kit](https://amzn.in/d/0exQsfGD)**                                                                           | OneWire  | GPIO 15                       | Waterproof temp probe. Kit includes the terminal adapter[cite: 7].          |
| **Built-in RGB LED**                                                                                                               | NeoPixel | GPIO 48                       | Onboard WS2812 used for system health visualization[cite: 7].               |

_\*Note: GPIO Pins can now be dynamically reassigned in the Web UI Settings tab._

> **⚡ Power Note:** Thanks to the V2 specifications of the DFRobot modules, **every single sensor in this project shares a unified 3.3V and GND rail**. No 5V logic-level shifting is required[cite: 7]!

---

## ⚙️ Arduino IDE Board Settings (ESP32-S3 N16R8)

To successfully flash this firmware, configure your Arduino IDE **Tools** menu exactly as follows[cite: 7]:

- **Board:** ESP32S3 Dev Module[cite: 7]
- **USB CDC On Boot:** Enabled[cite: 7]
- **CPU Frequency:** 240MHz (WiFi)[cite: 7]
- **Flash Mode:** QIO 80MHz[cite: 7]
- **Flash Size:** 16MB (128Mb)[cite: 7]
- **Partition Scheme:** 16M Flash (3MB APP/9.9MB FATFS) _(Required to fit the compiled Tailwind UI in LittleFS)_
- **PSRAM:** OPI PSRAM[cite: 7]
- **Upload Mode:** UART0 / Hardware CDC[cite: 7]

---

## 🚀 Setup & Installation

### 1. Library Dependencies

Install the following libraries via the Arduino Library Manager[cite: 7]:

- `FirebaseClient` by Mobizt (v2.2.13)[cite: 7]
- `ESP Async WebServer` by ESP32Async (v3.11.2)[cite: 7]
- `Async TCP` by ESP32Async[cite: 7]
- `ArduinoJson` by Benoit Blanchon (v7.4.3)[cite: 7]
- `ElegantOTA` by Ayush Sharma
- `Adafruit NeoPixel` by Adafruit[cite: 7]
- `BH1750` by Christopher Laws[cite: 7]
- `DHT sensor library` by Adafruit[cite: 7]
- `DallasTemperature` by Miles Burton[cite: 7]

### 2. Configure Fallback Credentials

1. Locate `example.secrets.h` in the root directory.
2. Copy and rename to `secrets.h`.
3. Populate it with baseline defaults. _Note: These are only used on the very first boot. Once you save settings via the Web UI, the NVS memory permanently overrides `secrets.h`._

### 3. Flash the Firmware

Choose one of the supported build paths:

- **Arduino IDE:** open the sketch in the IDE, select the ESP32-S3 board profile from the README, and click **Upload**.
- **PlatformIO (VS Code):** the project now includes a [platformio.ini](platformio.ini) file, so you can open the folder in VS Code with the PlatformIO extension and build/upload from the PlatformIO toolbar.

### 4. Upload the Web UI (LittleFS) - _CRITICAL STEP_

The C++ code alone will not serve the web interface[cite: 7]. You must upload the `data/` folder (which contains the compiled offline Tailwind CSS and Modular JS) to the ESP32's flash memory[cite: 7].

- Press `Ctrl+Shift+P` (or `Cmd+Shift+P`), type "Upload LittleFS to Pico/ESP8266/ESP32", and execute[cite: 7].

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

If one or more sensors disconnect or fail, the LED cycles through specific diagnostic colors[cite: 7]:

- 🔴 **Red**: Water Level failure[cite: 7]
- 🟡 **Yellow**: BH1750 Light failure[cite: 7]
- 🟣 **Purple**: TDS failure[cite: 7]
- 🟠 **Orange**: DHT22 failure[cite: 7]
- 🔵 **Blue**: pH Sensor failure[cite: 7]
- 🩵 **Cyan**: DS18B20 Water Temp failure[cite: 7]
- 🟢 **Green / Blue / Red (Solid)**: System Healthy (Water/Air Temp: Normal / Cold / Hot)[cite: 7]
