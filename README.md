# ESP32-S3 Modular Sensor → Firestore Pipeline

A robust, modular IoT firmware for the ESP32-S3 that reads data from 6 different water/environmental sensors and transmits the data asynchronously to Firebase Firestore.

## Features

- **6 Supported Sensors**: Water Level, Light (BH1750), TDS, Air Temp/Humidity (DHT22), pH, Water Temp (DS18B20).
- **Asynchronous Firestore Uploads**: Uses the modern `FirebaseClient` library to upload data without blocking sensor reads.
- **Built-in RGB Error Feedback**: Uses the ESP32-S3's onboard WS2812 NeoPixel to display specific color codes if a sensor disconnects or fails.
- **Vapor Pressure Deficit (VPD)**: Automatically calculates VPD in kPa based on air temperature and relative humidity.
- **Demo Mode**: A built-in mode to generate realistic mock data for testing your Firebase database without having physical sensors connected.
- **Anti-Ground Looping Sequence**: Sensors are read sequentially with intentional delays to minimize electrical interference between analog probes submerged in the same water.
- **Corrosion Resistance**: The analog water level sensor is only powered on during active reads to drastically reduce electrolytic corrosion on the copper traces.

---

## Hardware & Wiring

### Supported Board
- **ESP32-S3 N16R8** DevKit (or compatible ESP32-S3 boards)

### Pin Assignments
*(Configurable in `config.h`)*

| Sensor | Protocol / Type | ESP32-S3 Pin | Notes |
|---|---|---|---|
| **Water Level** | Analog | GPIO 1 | Signal pin |
| **Water Level Power** | Digital Out | GPIO 5 | Provides 3.3V only during active reading |
| **BH1750 Light** | I2C (SDA) | GPIO 8 | |
| **BH1750 Light** | I2C (SCL) | GPIO 9 | |
| **TDS Sensor** | Analog | GPIO 2 | |
| **DHT22** | Digital (1-Wire) | GPIO 6 | Temp & Humidity |
| **pH Sensor** | Analog | GPIO 20 | **Warning**: ADC2 pin. May conflict with WiFi on older ESP-IDF cores. |
| **DS18B20 Water Temp** | OneWire | GPIO 4 | **Requires a 4.7kΩ pull-up resistor** to 3.3V |
| **RGB LED** | NeoPixel | GPIO 48 | Built-in to the DevKit board |

> **Power Supply Note:** All sensors share a common 3.3V and GND, except for the pH sensor which may require 5V depending on your specific module. Check your sensor's datasheet.

---

## Operating Modes

This firmware features two modes, toggled via `#define DEMO_MODE` in `config.h`.

### Real Mode (`DEMO_MODE 0`)
- **Interval**: Data is sent to Firestore every **20 seconds**.
- **Execution**: Physical sensors are initialized and read.
- **Sequential Reading**: To reduce electrical interference (ground loops) between the TDS, pH, and Water Level sensors when submerged in the same liquid, the code enforces a `500ms` delay between analog reads. 

> **Important Note on Ground Loops:** While a software delay helps mitigate instantaneous CPU and ADC noise, it does **not** electrically isolate the probes. If your pH and TDS readings fluctuate wildly when placed in the same water, you will need a hardware analog isolator (e.g., DFRobot Gravity Analog Signal Isolator) for true electrical isolation.

### Demo Mode (`DEMO_MODE 1`)
- **Interval**: Data is sent to Firestore every **2 seconds**.
- **Execution**: Physical hardware reads are completely bypassed. The ESP32 generates realistic, slightly randomized mock data for all 6 sensors.
- **Purpose**: Ideal for testing UI, database structure, and connectivity without needing to wire up the entire hardware array.

---

## Vapor Pressure Deficit (VPD)

The firmware automatically calculates the Vapor Pressure Deficit (VPD) in **kPa**. VPD is a crucial metric for greenhouse and indoor agriculture that measures the difference between the amount of moisture in the air and how much moisture the air can hold when it is saturated.

The calculation uses the Tetens formula:
1. `SVP = 0.61078 * exp((17.27 * T) / (T + 237.3))`
2. `AVP = SVP * (RH / 100.0)`
3. `VPD = SVP - AVP`

---

## LED Error Color Codes

The built-in NeoPixel LED gives immediate visual feedback on system health. If multiple sensors fail, the LED will cycle through their respective colors.

- 🔴 **Red**: Water Level failure
- 🟡 **Yellow**: BH1750 Light failure
- 🟣 **Purple**: TDS failure
- 🟠 **Orange**: DHT22 failure
- 🔵 **Blue**: pH Sensor failure
- 🩵 **Cyan**: DS18B20 Water Temp failure
- ⚪ **White (Blinking)**: Firebase / WiFi connection failure
- 🟢 **Green (Solid)**: All systems operational

---

## Setup & Installation

### 1. Configure Firebase
1. Create a project in the [Firebase Console](https://console.firebase.google.com/).
2. Navigate to **Firestore Database** and create a database (Start in Test Mode).
3. Navigate to **Authentication**, enable the **Email/Password** provider, and add a test user.
4. Get your **Web API Key** from Project Settings.

### 2. Configure the Code
Open `config.h` and populate your credentials:
```cpp
#define WIFI_SSID              "YOUR_WIFI"
#define WIFI_PASSWORD          "YOUR_PASSWORD"
#define FIREBASE_API_KEY       "YOUR_API_KEY"
#define FIREBASE_PROJECT_ID    "YOUR_PROJECT_ID"
#define FIREBASE_USER_EMAIL    "test@example.com"
#define FIREBASE_USER_PASSWORD "password123"
```

### 3. Install Required Libraries
In the Arduino IDE, open **Sketch > Include Library > Manage Libraries** and install:
- `FirebaseClient` by Mobizt
- `Adafruit NeoPixel` by Adafruit
- `BH1750` by Christopher Laws
- `DHT sensor library` by Adafruit (and `Adafruit Unified Sensor`)
- `OneWire` by Paul Stoffregen
- `DallasTemperature` by Miles Burton

### 4. Upload
Select **ESP32S3 Dev Module** in the Arduino IDE. Ensure **USB CDC On Boot** is enabled so you can view the Serial Monitor logs at `115200` baud.
