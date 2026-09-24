# HyGrow IoT

HyGrow IoT is ESP32-S3 firmware for six hydroponic sensors. The board hosts
its own web dashboard, so you can view readings and change settings on a
local connection. Internet access is only needed for optional Firestore
uploads.

Sensor reading and networking run on separate FreeRTOS cores. Settings,
calibration, and pin assignments are saved on the board across normal reboots.

## What you need

- An **ESP32-S3 N16R8** board (16 MB flash, 8 MB PSRAM).
- The sensors you plan to use. You can start with **Demo Mode** before wiring
  them.
- [PlatformIO](https://platformio.org/) (recommended), or an Arduino IDE
  with the ESP32-S3 board package and the libraries listed in
  [`platformio.ini`](platformio.ini).
- Python 3 to refresh the compressed dashboard files before a filesystem
  upload.

## Default wiring

| Sensor and example purchase link | Protocol | Default ESP32-S3 pins | Note |
| --- | --- | --- | --- |
| [Water level sensor](https://amzn.in/d/0cKf4nuQ) | Analog | GPIO1 signal, GPIO5 switched power | Power is switched on briefly for each reading to reduce corrosion. |
| [BH1750 light sensor](https://amzn.in/d/09NZHxCq) | I2C | GPIO8 SDA, GPIO9 SCL | Ambient light sensor. |
| [DFRobot Gravity analog TDS](https://robocraze.com/products/dfrobot-gravity-analog-tds-water-quality-sensor-meter-for-arduino) | Analog | GPIO2 signal | Readings are median-filtered in firmware. |
| [Hexonix DHT22 AM2302](https://amzn.in/d/07a1dbpF) | Digital | GPIO6 data | Air temperature and humidity. |
| [DFRobot Gravity Lab pH V2](https://robu.in/product/dfrobot-gravity-lab-grade-analog-ph-sensor-meter-kit-v2/) | Analog | GPIO7 signal | Starts disabled until enabled and calibrated; supports 3.3 V. |
| [amiciSense DS18B20 kit](https://amzn.in/d/0exQsfGD) | OneWire | GPIO4 data | Waterproof water-temperature probe. |
| Built-in RGB LED | NeoPixel | GPIO48 (reserved) | Onboard WS2812 status light. |

The sensor modules use a common 3.3 V supply and ground. The pH sensor starts
disabled. You can change sensor pins and enabled states in **Settings → Sensor Implementation
Config** after connecting to the device.

The dashboard and firmware reject pins reserved for boot, USB, flash/PSRAM,
or the status LED. TDS, pH, and the water-level signal must use ADC1 pins
GPIO1–10. The limits follow the [ESP32-S3 DevKitC-1 board guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html)
and [hardware pin guidance](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html).

## First setup

1. Copy [`example.secrets.h`](example.secrets.h) to `secrets.h` in the project
   root. Fill in the values you want at first boot. The SoftAP recovery
   password must be empty (open network) or at least eight characters. An
   empty admin password starts first-time password setup in the dashboard.
   `secrets.h` is ignored by Git. These defaults are read again after a
   factory reset.
2. Build the firmware with PlatformIO:

   ```powershell
   pio run -e esp32-s3-n16r8
   ```

3. Generate the dashboard's compressed files, then build and upload its
   LittleFS image:

   ```powershell
   python tools/build-web-assets.py
   pio run -e esp32-s3-n16r8 -t uploadfs
   ```

4. Upload the firmware:

   ```powershell
   pio run -e esp32-s3-n16r8 -t upload
   ```

5. If the board cannot join Wi-Fi, connect your phone or computer to its
   `HyGrow-Setup` network and open **http://192.168.4.1**. If it joined your
   configured Wi-Fi, use the board's address on that network instead. Set or
   enter the admin password when prompted.

The dashboard and firmware are uploaded separately. If you only upload the
firmware, the web page may still be an older version. The board keeps saved
settings in NVS across normal reboots.

### Using Arduino IDE

Select **ESP32S3 Dev Module** with these board settings:

| Setting | Value |
| --- | --- |
| USB CDC on boot | Enabled |
| CPU frequency | 240 MHz |
| Flash mode | QIO 80 MHz |
| Flash size | 16 MB |
| Partition scheme | 16 MB flash, 3 MB app, about 9.9 MB filesystem |
| PSRAM | OPI PSRAM |
| Upload mode | UART0 / Hardware CDC |

Install the dependencies from [`platformio.ini`](platformio.ini). Upload the
sketch and use a LittleFS upload plugin to flash `data/`. Run the same Python
asset command before the filesystem upload.

For Arduino IDE or CLI, install these libraries. PlatformIO installs them
from `platformio.ini` automatically.

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

Arduino CLI can compile with the matching board settings:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3:UploadSpeed=115200,USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CPUFreq=240,UploadMode=default .\HyGrow_IoT.ino
```

## Using the dashboard

- **Dashboard:** view current readings and device connection state. A green
  sensor dot means a healthy real reading; purple means simulated Demo Mode;
  red means a read error; gray means disabled or device offline. A waiting
  sensor uses amber until its first successful reading. A missing or failed
  reading shows `--` instead of an old value.
- **Sensor pages:** see a sensor's reading, pin, state, Demo Mode switch, and
  power switch. A sensor can be enabled yet still have a read error.
- **Calibration:** use the one-point TDS and two-point pH guides, but only
  after a healthy real reading.
  Demo, disabled, failed, and offline sensors cannot be calibrated.
- **Settings:** change Wi-Fi, password, cloud credentials, Demo Mode, sensor
  pins, and timing. An **Unsaved changes** control stays visible across pages
  when Settings edits are pending; open it to save or discard each section.
- **Terminal:** see device logs, including sensor errors and reboot reasons.

Global Demo Mode simulates every sensor and requires a reboot to apply.
Turning it off leaves all sensors enabled. Individual sensor pages also have
a Demo Mode switch for that sensor. The dashboard reports **rebooting** and
then **reconnected** when a requested restart completes. A banner explains
reconnect attempts if the board remains offline. You can close the connecting
screen to inspect the offline dashboard; device actions still need a live
connection.

**Network recovery:** if the board cannot join your saved Wi-Fi, it starts
the `HyGrow-Setup` network. Reconnect to it and return to 192.168.4.1 to
correct the settings.

## Cloud upload (optional)

The device can update one Firestore document at `devices/{device_id}` by
default. You can change the collection in Cloud Provisioning. The document
holds current readings and status, **not** a reading history. A disabled or
failed sensor sends `null` for its reading; it does not make the whole device
offline. `uptime_s` is measured in seconds since the current boot.

1. Create a Firebase project and enable Firestore.
2. Enable Email/Password sign-in and create a device account.
3. Set Firestore rules that restrict this account to its device document.
4. In **Settings → Cloud Provisioning**, enter the Project ID, Web API Key,
   email, password, and collection. Save, then use **Test Connection**.
5. Enable **Firebase Upload** in the same card and save it. The default upload
   interval is 10 seconds; you can change it in Settings.

The device cannot write an “Offline” update after it loses power or network
access. A separate reader must compare `lastUpdated` against the current
time to detect stale devices. A failed sensor does not mean the whole device
is offline.

## Recovery and troubleshooting

| Problem | What to do |
| --- | --- |
| Forgot the admin password | Hold the board's BOOT button for 10 seconds to reset only authentication. |
| Need a full reset | Hold BOOT for 20 seconds, or use **Factory Reset** in Settings and type `RESET`. This erases saved Wi-Fi, cloud, pins, calibration, and password. |
| Sensor disabled after boot | Check wiring. Then use that sensor's **Reset** action in Settings or enable it and save. The firmware can disable a sensor after five failed startup attempts. |
| Dashboard missing or old | Regenerate compressed assets and upload the LittleFS image again. |
| Filesystem mount failed | Reflash the LittleFS image. The onboard LED shows solid magenta at boot for this problem. |

### Onboard LED signals

| LED | Meaning |
| --- | --- |
| Off | All enabled sensors read successfully. |
| Solid red | Water level failed. |
| Solid yellow | Light sensor failed. |
| Solid purple | TDS failed. |
| Solid orange | DHT22 failed. |
| Solid blue | pH failed. |
| Solid cyan | Water temperature failed. |
| Fast white strobe | Two or more enabled sensors failed. |
| Solid magenta at boot | LittleFS mount failed; reflash the filesystem image. |

Disabled sensors do not count as failures.

## For future developers

### Regenerate web assets every time the dashboard changes

The board may serve `.gz` copies instead of the source files. **After editing
HTML, CSS, JavaScript, or Material Symbols, regenerate their compressed
files before building or uploading LittleFS:**

```powershell
python tools/build-web-assets.py
pio run -e esp32-s3-n16r8 -t buildfs
```

The script regenerates `index.html.gz`, `style.css.gz`, `app.js.gz`,
`charts.js.gz`, and `symbols.woff2.gz`, then checks that
each expands to the current source. When adding or changing icon names,
update the font with [`tools/build-icon-font.py`](tools/build-icon-font.py)
first; that script requires an upstream Material Symbols font path (see its
header for the exact command). Keep asset filenames short enough for
LittleFS; the longer former font filename could not be packaged with `.gz`.

### Where code lives

| Path | Purpose |
| --- | --- |
| [`HyGrow_IoT.ino`](HyGrow_IoT.ino) | Boot sequence and task startup |
| [`config.h`](config.h) | Sensor IDs, default pins, and intervals |
| [`example.secrets.h`](example.secrets.h) | Template for first-boot private settings |
| [`platformio.ini`](platformio.ini) | Board, libraries, and build settings |
| [`partitions.csv`](partitions.csv) | App and LittleFS flash layout |
| [`src/core/`](src/core/) | State, authentication, commands, networking, cloud sync, and sensor task |
| [`src/sensors/`](src/sensors/) | Hardware reads for each sensor |
| [`src/utils/`](src/utils/) | Onboard status LED |
| [`data/index.html`](data/index.html) | Dashboard markup |
| [`data/js/app.js`](data/js/app.js) | Dashboard state and interactions |
| [`data/css/style.css`](data/css/style.css) | Dashboard styles |
| [`data/manifest.json`](data/manifest.json) | Web app manifest |

[`PROGRESS.md`](PROGRESS.md) has older project notes. Check the current code
when a note and the implementation differ.

## License

This repository has no license file yet. Add one before redistributing the
project if you want to state what others may do with the code.
