# breezedudeGS  
Basic FANET OGN Groundstation based on ESP32 + SX1262

## BETA
Note: This project is currently in beta and may require further testing. If you encounter any issues or unexpected behavior, please open an issue or let me know.

## Why another groundstation software?

Currently, the main groundstation options are [GxAirCom](https://github.com/gereic/GXAirCom) and [SDR-based OGN](https://github.com/glidernet/ogn-rf) receivers.  
As long as the OGN receiver software remains closed source and does some shady deals with FLARM, I will not support it. FLARM reception is also not intended in this project.

Both GxAirCom and SDR-based solutions only publish data to OGN, which has some disadvantages — such as reduced accuracy (e.g., windspeed conversion to integer knots) and missing data like battery state of charge.  

GxAirCom technically has features to address these issues, but it is overloaded: it can be used as a variometer, flying device, display unit, and cellular modem. The result is a bloated codebase with limited maintenance. Some features are broken or hard to fix.

So, why not start from scratch?  
This project is a **simple groundstation** that acts purely as a **gateway for FANET data to the internet** — nothing more. It includes useful features like over-the-air updates via a web interface and live viewing of received data.

## Hardware support

Currently designed for the **[Heltec Wireless Stick V3 Lite](https://heltec.org/project/wireless-stick-lite-v2/)** a **ESP32-S3 + SX1262** board.  
Multicore is not explicitly required. The project uses **[RadioLib](https://github.com/jgromes/RadioLib)** for flexible radio support.

## Features

- Receives FANET weather data, live tracking, and names  
- Sends received FANET data to APRS servers like OGN
- Optionally posts data to an HTTP API endpoint (e.g., breezedude server)  
- Simple and modern web UI with captive portal and WebSocket live data
- Web updater (firmware & web)

## Not Features

- No FANET frame forwarding — this is an internet gateway; who would receive them? Everyone flies with a phone  
- No third-party weather station data sent to FANET  
- Not a weather station: no display, no external sensors  
- Wi-Fi only for now — cellular support may be added later (only for solar-powered hardware)

If you miss some features let me know.

# Setup
Requirements: 
- VSCode with PIO installed
- NodeJs (for minifying HTML, optional)

PIO will setup everything required. 
`pre:install_deps.py` will run `npm i` in `/bundle` for minifying HTML+JS

## Configuration
- (optional) edit `src/config.h` to set your default values.
- Flash device and connect to WiFi AP `BD-Groundstation` default password is `configureme`
- Follow captive portal or open `192.168.4.1`
- Set up your values for WiFi, name and position
- If everything is set up correctly, enable sending data to OGN by filling `aprs.glidernet.org` in APRS Server. Port is 14580

## Uploading

### Using VSCode & PIO
Just click `Upload`.
It is required to upload the SPIFFS/LittleFS image as well. For this click `Upload Filesystem Image` under `Platform` in PIO Poject Tasks.   
If you want to skip rebundeling HTML+JS comment `pre:minify.py` in extra scripts with a semicolon

### Using Web Update
the script `post:copy_firmware.py` wil runn automatically on build and generate and copy the files for web-update to the project root
- In WebUI -> Tools select & upload `firmware_update.bin`
- In WebUI -> Tools select & upload `littlefs.bin`
This should work most of the time. If some changes break the updater, an upload with PIO is required.

-----

If you like this project

<a href="https://www.buymeacoffee.com/thezenox" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me A Landingbeer" style="height: 60px !important;width: 200px !important;" ></a>
