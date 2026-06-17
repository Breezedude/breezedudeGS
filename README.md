# 📡 breezedudeGS  
Basic FANET OGN Groundstation based on ESP32 + SX1262

## ⁉️ Why another groundstation software?

Currently, the main groundstation options are [GxAirCom](https://github.com/gereic/GXAirCom) and [SDR-based OGN](https://github.com/glidernet/ogn-rf) receivers.  
As long as the OGN receiver software remains closed source and does some shady deals with FLARM, I will not support it. FLARM reception is also not intended in this project.

Both GxAirCom and SDR-based solutions only publish data to OGN, which has some disadvantages — such as reduced accuracy (e.g., windspeed conversion to integer knots) and missing data like battery state of charge.  

GxAirCom technically has features to address these issues, but it is overloaded: it can be used as a variometer, flying device, display unit, and cellular modem. The result is a bloated codebase with limited maintenance. Some features are broken or hard to fix.

So, why not start from scratch?  
This project is a **simple groundstation** that acts purely as a **gateway for FANET data to the internet** — nothing more. It includes useful features like over-the-air updates via a web interface and live viewing of received data.

## ⚙️ Hardware support

Currently designed for the **[Heltec Wireless Stick V3 Lite](https://heltec.org/project/wireless-stick-lite-v2/)** a **ESP32-S3 + SX1262** board.  
Where to buy:    
* [DE - OpenElab](https://openelab.io/de/products/heltec-wireless-stick-lite-esp32s3)
* [CN - Aliexpress (868MHz)](https://de.aliexpress.com/item/1005006340203806.html)

Multicore is not explicitly required. The project uses **[RadioLib](https://github.com/jgromes/RadioLib)** for flexible radio support.

## ✅ Features

- Receives FANET weather data, live tracking, and names  
- Sends received FANET data to APRS servers like OGN
- Optionally posts data to an HTTP API endpoint (e.g., breezedude server)  
- Simple and modern web UI with captive portal and WebSocket live data
- Web updater (firmware & web)
- Remote configuration updates for Breezedude wind sensors via LoRa
- Over-the-air firmware updates for Breezedude devices

## 🚫 Not Features

- No FANET frame forwarding — this is an internet gateway; who would receive them? 
- No third-party weather station data sent out to FANET  
- Not a weather station, no display, no external sensors  
- Wi-Fi only for now — cellular support may be added later (only for solar-powered hardware)

If you miss some features let me know.

## Power Consuption
using a stock Heltec Wireless Stick V3 Lite:
- BreezedudeGS AP+STA: avg. 124mA @ 5V
- BreezedudeGS STA: avg. 59mA @ 5V


## Installing Using WebFlasher (recommended)
Visit [Breezedude Web Installer](https://install.breezedude.de/tools/groundstation-install.html) (requires Chrome or a [WebSerial compatible](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API#browser_compatibility) browser on desktop)
1. Connect the Heltec Wireless Stick to your computer using a USB C cable

2. Click Connect. A popup Window opens in yur browser. You should see a CP2102 device in the list. Seldt it an continue     
<img width="350" alt="image" src="https://github.com/user-attachments/assets/3b85e4ce-6576-4db2-aef7-6748996edf04" />
     
3. Wait for the installation to finish. During flashing you can enter the stations settings, it will be applied afterwards.
<img width="350" src="https://github.com/user-attachments/assets/db24c6b9-1a39-4978-90d6-e772b184b3cb" />

5. If you did not setup the WiFi directly, you should find a "BD-Groundstation" WiFi. Connect using the default password `configureme`. Continue with the Configuration
<img width="150" alt="image" src="https://github.com/user-attachments/assets/b4301a59-db89-4be0-bde2-599cbccdac62" />

## 🎚️ Configuration
- Flash device and connect to WiFi AP `BD-Groundstation` default password is `configureme`
- Follow captive portal or open [192.168.4.1](http://192.168.4.1) if you get not directed automatically
- Set up your values for WiFi, name and position
- If everything is set up correctly, enable sending data to OGN by enableing the checkbox and filling `aprs.glidernet.org` in APRS Server. Port is 14580

> [!IMPORTANT]
> Make sure to set a proper location and name.

If something went wrong, you can reset to factory config by pressing and holding user button *shortly after* pressing Reset button. It will be confirmed by a short blink of the white onboard LED. If pressing user button while pressing reset button you will enter esp download mode.

For debugging you may connect to your device using [Breezedude Web Installer](https://install.breezedude.de/) and open the console by clicking `Logs & Console`

## 🔧 Remote Configuration Updates

The Ground Station automatically polls the Breezedude backend for pending configuration updates for all known devices.

**How it works:**
1. Configuration changes and firmware selection can be made in a UI (coming soon) and will be stored in the Database
2. Breezedude sends firmware version to Ground Station. Groundstation requests server for update.
3. If any updates are pending the GS receives a signed packet and a unique `session_id`
4. The packet is transmitted via LoRa to the target device
5. The target device checks the signature and applies changes to flash. Acceptance is confirmed to Ground Station.
5. GS reports the confirmation to backend using the `session_id`

**Security:**
- All config packets are cryptographically signed by the backend (ECDSA P-256)
- Each device verifies the signature using its unique UUID suffix
- Session IDs prevent API spoofing attacks

**Monitoring:**
- Config and firmware update activities are logged to serial output and web console.

## ↗ Updating
### Using Auto-update (recommended)
Select `stable` or `beta` channel in the stations webinterface. It will automatically check for updates and install them.
### Manual Web Update
Get the current version from [releases](https://github.com/thezenox/breezedudeGS/releases)
Open configuration page of breezedude groundstation
- (optional) Backup the settings by clicking `Export Config` in Tools
- In Tools select 'Update' and upload `littlefs.bin`
- In Tools select 'Update' and upload `firmware_update.bin`

(optional) Restore Config if its broken/reset by uploading your backup file. Don't forget to click `Save` to write the uploaded settings
This should work most of the time. If some changes break the updater, an clean upload with WebFlasher/PIO may be required.

### Using Fresh install
You can write a fresh firmware image for updating. Either using the [Breezedude Web Installer](https://install.breezedude.de/) or Building & Uploading with PIO. This may be required if the integrated web updater fails updating.

## 🛠 Build from source using VSCode & PIO

Requirements: 
- VSCode with PIO installed
- NodeJs (for minifying HTML, optional)

Steps:
- Clone/Download this repo and open it in VSCode with PlatformIO installed
- PIO will setup everything required. 
- (optional) edit `src/config.h` to set your default values.
- In PIO Project Tasks open `heltec_wifi_lora_32_V3` -> `General` and click `Upload`.
- It is required to upload the SPIFFS/LittleFS image as well. For this click `Upload Filesystem Image` under `Platform` in PIO Poject Tasks.   

> [!NOTE]
> If you want to skip rebundeling HTML+JS comment `pre:minify.py` in extra scripts with a semicolon   
> `pre:install_deps.py` will run `npm i` in `/bundle` for minifying HTML+JS

> [!NOTE]
> The script `post:copy_firmware.py` will run automatically on build and generate and copy the files for web-update to the project root.    
> The custom Task `Build & Bundle all` will generate the all-in-one.bin for webflashing (saved to the project root).

-----

If you like this project

<a href='https://ko-fi.com/P5P41RYRWK' target='_blank'><img height='36' style='border:0px;height:36px;' src='https://storage.ko-fi.com/cdn/kofi5.png?v=6' border='0' alt='Buy Me a Coffee at ko-fi.com' /></a>
