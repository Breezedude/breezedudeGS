# 📡 Breezedude Groundstation

A FANET ground station for ESP32-S3 boards: it receives weather stations and
live tracking over the air and brings them to the internet - to the Breezedude
server and to OGN.

## Why a ground station of our own?

Breezedude wind sensors are more than data sources. They are updated over the
air, configured remotely and monitored in the field, and all of that runs
through the ground stations near them. Existing ground station software only
forwards data to OGN - with reduced accuracy (wind rounded to whole knots) and
without battery state, firmware updates or remote configuration.

This firmware is what makes the Breezedude ecosystem possible: it is the link
between the sensors on the hill and the server, in both directions.

## Features

**Reception**
- Made for FANET: weather stations, live tracking, names, device info
- Also decodes FLARM and ADS-L air traffic (optional)
- Forwards to the Breezedude server and to OGN (APRS)

**Breezedude sensors**
- Over-the-air firmware updates for sensors in range, with live progress and
  an analysis of every update attempt
- Remote configuration of sensors
- Repeater monitoring: which packets arrive directly, which only through a
  repeater

**Station**
- ESP-IDF with automatic light sleep - about 15 mA at 5 V instead of 60 mA
  with the previous firmware, suitable for solar and battery stations
- Battery monitoring and an optional night-time switch-off
- Web interface with live data, 24 h statistics, reception quality and
  system console
- Settings can be protected by a password; everyone in the network can still
  view everything
- Remote monitoring in the [device portal](https://devices.breezedude.de) -
  link the station under **System → Breezedude portal**
- Automatic firmware updates with rollback if a new version does not start

## Hardware

Designed for the **Heltec Wireless Stick Lite V3** (ESP32-S3 + SX1262).
Other Heltec V3 boards with ESP32-S3 and SX1262 or SX1276 and 4 MB or 8 MB flash
work as well.

Where to buy:
- [DE - OpenElab](https://openelab.io/de/products/heltec-wireless-stick-lite-esp32s3)
- [CN - AliExpress (868 MHz)](https://de.aliexpress.com/item/1005006340203806.html)

## Installation

Use the web installer - no tools needed, only Chrome or Edge on a desktop:

👉 **[install.breezedude.de - Groundstation](https://install.breezedude.de/tools/groundstation-install.html)**

Name, position and WiFi can be entered right there during flashing.

## Setup

If you did not enter the WiFi during installation:

1. Connect to the WiFi **`BD-Groundstation`**, password **`configureme`**
2. The setup page opens by itself, otherwise open [192.168.4.1](http://192.168.4.1)
3. Enter your WiFi, the station's name and its exact position, then save

> [!IMPORTANT]
> Forwarding to OGN and Breezedude only starts once a name and a real
> position are set.

## Updates

Turn on **automatic updates** under **System → Firmware** - recommended. The station then keeps
itself up to date; a version that does not start properly is rolled back
automatically.

| Channel | For whom |
|---|---|
| **stable** | Everyone. Thoroughly tested - the carefree choice |
| **beta** | Gets new versions earlier |
| **alpha** | Developers and testers only - may break things |

Stations running the previous Arduino firmware (0.6.x) update to this one
automatically and keep their settings.

Manual update: download `firmware.bin` and `www.bin` from the
[releases](https://github.com/Breezedude/breezedudeGS/releases) and upload them
under **System → Firmware → Manual update** - `www.bin` first, then
`firmware.bin` (the station restarts after it).

## Help

- **Forgot the password?** Unlock with the button on the station, then set a
  new one under **Settings → Limit access**.
- **Reset all settings:** press and hold the user button right after pressing
  the reset button, until the white LED blinks.
- **Logs:** connect the station by USB and open *Logs & Console* in the
  [web installer](https://install.breezedude.de/).

Developers: building from source and internals are described in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## License

Non-commercial use, CC BY-NC 4.0 - see [LICENSE.txt](LICENSE.txt).

-----

If you like this project

<a href='https://ko-fi.com/P5P41RYRWK' target='_blank'><img height='36' style='border:0px;height:36px;' src='https://storage.ko-fi.com/cdn/kofi5.png?v=6' border='0' alt='Buy Me a Coffee at ko-fi.com' /></a>
