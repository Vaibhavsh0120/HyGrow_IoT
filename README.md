# HyGrow-IoT: Advanced ESP32-S3 Dual-Core Sensor Pipeline

![ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-blue)
![FreeRTOS](https://img.shields.io/badge/OS-FreeRTOS-orange)
![Firebase](https://img.shields.io/badge/Database-Firestore-yellow)
![Status](https://img.shields.io/badge/Status-Active-success)

A robust, professional-grade IoT firmware for the ESP32-S3. HyGrow-IoT reads real-time environmental and hydroponic data from 6 different sensors, processes it asynchronously, and transmits it to Firebase Firestore.

This project utilizes a **Dual-Core FreeRTOS Architecture** and a **LittleFS-hosted Web Application** to provide a 100% offline-capable diagnostic dashboard, dynamic state configuration, and zero-blocking hardware reads.

---

## 🌟 Key Features

* **Dual-Core Processing (FreeRTOS):**
  * **Core 0:** Handles WiFi, the LittleFS Web Server, WebSockets, and asynchronous Firebase uploads.
  * **Core 1:** Exclusively dedicated to precise hardware timing, sensor reads, and VPD calculations without network-induced latency.
* **Offline-Capable SPA Web Dashboard:** A beautiful, responsive, dark-mode Single Page Application hosted directly on the ESP32's flash memory. View real-time graphs, system logs, and toggle sensors dynamically.
* **Dynamic NVS Configuration:** Toggle Demo Mode, turn individual sensors ON/OFF, and update WiFi credentials via the Web UI. Settings are saved to Non-Volatile Storage (NVS) and persist across reboots.
* **Asynchronous Firestore Uploads:** Uses the modern `FirebaseClient` library to stream dynamic payloads to the cloud.
* **Smart RGB Error Feedback:** Uses the ESP32-S3's onboard WS2812 NeoPixel to display system health and cycle through specific color codes for disconnected/failed hardware.
* **Vapor Pressure Deficit (VPD):** Automatically calculates VPD (kPa) based on air temperature and relative humidity for precision greenhouse monitoring.

---

## 🛠 Hardware & Wiring

**Supported Board:** ESP32-S3 N16R8 DevKit

This project is optimized for modular sensor kits that include built-in resistors and signal conditioning (such as terminal adapters and pull-ups). This allows for **direct connection** to the ESP32-S3 without requiring additional breadboard circuitry.

### Specific Sensor Hardware Used

| Sensor Module & Purchase Link | Protocol | ESP32-S3 Pin | Notes |
|---|---|---|---|
| **[Water Level Sensor](https://amzn.in/d/0cKf4nuQ)** | Analog | GPIO 1 (Sig)<br>GPIO 5 (Pwr) | Powered dynamically via GPIO 5 to drastically reduce electrolytic corrosion on the copper traces. |
| **[BH1750 Light Sensor](https://amzn.in/d/09NZHxCq)** | I2C | GPIO 8 (SDA)<br>GPIO 9 (SCL) | Digital ambient light detection. |
| **[DFRobot Gravity Analog TDS](https://robocraze.com/products/dfrobot-gravity-analog-tds-water-quality-sensor-meter-for-arduino)** | Analog | GPIO 2 | Uses 30-sample median filtering in code for noise reduction. |
| **[Hexonix DHT22 AM2302](https://amzn.in/d/07a1dbpF)** | Digital | GPIO 6 | Temperature & Humidity. Module includes built-in pull-up resistor. |
| **[DFRobot Gravity Lab pH V2](https://robu.in/product/dfrobot-gravity-lab-grade-analog-ph-sensor-meter-kit-v2/)** | Analog | GPIO 20 | Lab-grade analog pH sensing. Native 3.3V support! *(Warning: ADC2 pin)* |
| **[amiciSense DS18B20 Kit](https://amzn.in/d/0exQsfGD)** | OneWire | GPIO 4 | Waterproof temp probe. Kit includes the terminal adapter which houses the necessary 4.7kΩ pull-up resistor. |
| **Built-in RGB LED** | NeoPixel | GPIO 48 | Onboard WS2812 used for system health visualization. |

> **⚡ Power Note:** Thanks to the V2 specifications of the DFRobot modules, **every single sensor in this project shares a unified 3.3V and GND rail**. No 5V logic-level shifting is required, allowing for incredibly clean and straightforward wiring directly to the ESP32-S3's 3.3V output!

---

## ⚙️ Arduino IDE Board Settings (ESP32-S3 N16R8)

To successfully flash this firmware and utilize the Web UI on the N16R8 variant, configure your Arduino IDE **Tools** menu exactly as follows:

* **Board:** ESP32S3 Dev Module
* **USB CDC On Boot:** Enabled *(Required to see the Web UI IP address in Serial Monitor)*
* **CPU Frequency:** 240MHz (WiFi)
* **USB DFU On Boot:** Disabled
* **Erase All Flash Before Sketch Upload:** Enabled *(Recommended for fresh installs)*
* **Events Run On:** Core 0
* **Flash Mode:** QIO 80MHz
* **Flash Size:** 16MB (128Mb)
* **JTAG Adapter:** Disabled
* **Arduino Runs On:** Core 1
* **USB Firmware MSC On Boot:** Disabled
* **Partition Scheme:** Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)
  *(💡 Pro-Tip: Because you have a 16MB board, you can safely upgrade this to "16M Flash (3MB APP/9.9MB FATFS)" to unlock massive storage for future Web UI updates).*
* **PSRAM:** OPI PSRAM *(Crucial for the N16R8 board)*
* **Upload Mode:** UART0 / Hardware CDC
* **Upload Speed:** 921600
* **USB Mode:** Hardware CDC and JTAG

---

## 🚀 Setup & Installation

### 1. Library Dependencies
Open **Sketch > Include Library > Manage Libraries** and install the following libraries. The versions listed below are the exact versions tested and verified for this firmware:

**Core Frameworks & Networking:**
* `FirebaseClient` by Mobizt (v2.2.13)
* `ESP Async WebServer` by ESP32Async (v3.11.2)
* `Async TCP` by ESP32Async (v3.4.10) - *Dependency for WebServer*
* `ArduinoJson` by Benoit Blanchon (v7.4.3)

**Hardware & Sensors:**
* `Adafruit NeoPixel` by Adafruit (v1.15.5)
* `BH1750` by Christopher Laws (v1.3.0)
* `DHT sensor library` by Adafruit (v1.4.7)
* `Adafruit Unified Sensor` by Adafruit (v1.1.15) - *Dependency for DHT*
* `DallasTemperature` by Miles Burton (v4.0.6)
* `OneWire` by Paul Stoffregen (v2.3.8) - *Dependency for DallasTemperature*

### 2. Configure Credentials (Securely)
This project uses a standard `.gitignore` approach to keep your credentials safe.
1. Locate `secrets.h.example` in the root directory.
2. Copy the file and rename the copy to `secrets.h`.
3. Populate `secrets.h` with your WiFi and Firebase credentials.
*(Note: `secrets.h` is ignored by Git, ensuring you never accidentally commit your passwords).*

### 3. Flash the Firmware
Click the standard **Upload** arrow in the Arduino IDE to compile and upload the C++ code to the ESP32.

### 4. Upload the Web UI (LittleFS) - *CRITICAL STEP*
The C++ code alone will not serve the web interface. You must upload the `data/` folder to the ESP32's flash memory.
* **Arduino IDE v2.x:** Press `Ctrl+Shift+P` (or `Cmd+Shift+P`), type "Upload LittleFS to Pico/ESP8266/ESP32", and execute. *(Requires the [arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload) extension).*
* **PlatformIO:** Run the `Upload Filesystem Image` task.

---

## 💻 The Web Diagnose Dashboard

Once booted, open the Arduino Serial Monitor (115200 baud). The ESP32 will output an IP address (e.g., `http://192.168.1.50`). Open this in your browser to access the SPA Dashboard.

* **Real-Time Graphs:** View high-speed Canvas charts of your sensor data streamed via WebSockets.
* **Hardware Toggles:** Turn physical sensors ON/OFF. The C++ backend instantly stops reading disabled sensors, and the Firebase payload dynamically shrinks to save bandwidth.
* **Terminal:** View system logs, FreeRTOS cross-core events, and Firebase upload statuses entirely within the browser.
* **Demo Mode:** Toggle Demo Mode via the settings page to generate realistic mock data (useful for testing UI/Database structures without water).

---

## 🌈 LED Error Color Codes

The onboard NeoPixel continuously monitors hardware health. If one or more sensors disconnect or fail, the LED cycles through specific diagnostic colors:

* 🔴 **Red**: Water Level failure
* 🟡 **Yellow**: BH1750 Light failure
* 🟣 **Purple**: TDS failure
* 🟠 **Orange**: DHT22 failure
* 🔵 **Blue**: pH Sensor failure
* 🩵 **Cyan**: DS18B20 Water Temp failure
* 🟢 **Green / Blue / Red (Solid)**: System Healthy (Indicates Water/Air Temperature: Normal / Cold / Hot)
