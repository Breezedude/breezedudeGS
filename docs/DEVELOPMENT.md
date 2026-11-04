# Breezedude Groundstation - development notes

FANET / FLARM / ADS-L receiving ground station for ESP32-S3 (primary) and
ESP32-C6, built on ESP-IDF 5.5 with FreeRTOS tasks and automatic light sleep.

Successor of the Arduino firmware (branch `arduino`) - same web UI,
same websocket protocol, same APRS/OGN uplink, same LoRa OTA gateway for
Breezedude sensors. What changed is the foundation:

| | Arduino build | this port |
|---|---|---|
| Framework | arduino-esp32, `loop()` | ESP-IDF 5.5, tasks |
| Idle behaviour | busy loop at 80 MHz | DFS 40-160 MHz + automatic light sleep |
| Web server | ESPAsyncWebServer + AsyncTCP | `esp_http_server` |
| Flash | 8 MB, 2x 3.3 MB app | **4 MB or 8 MB, 2x 1.6 MB app, sensor image store** |
| Self update | esp32FOTA | `esp_https_ota` + manifest parser |
| Targets | ESP32-S3 | ESP32-S3, ESP32-C6 |

## Build

ESP-IDF 5.5 must be installed:

```bash
. $IDF_PATH/export.sh            # export.ps1 in PowerShell
idf.py set-target esp32s3
idf.py build flash monitor
```

ESP32-C6 uses its own build directory so both targets can coexist:

```bash
idf.py -B build.c6 -D SDKCONFIG=sdkconfig.c6 set-target esp32c6
idf.py -B build.c6 -D SDKCONFIG=sdkconfig.c6 build flash monitor
```

Board pins, the radio feature set and the power policy live in
`idf.py menuconfig` under **Breezedude GS board / features / power management**.
Defaults are the Heltec WiFi LoRa 32 V3 pin map; the C6 defaults are placeholders
(see `main/Kconfig.projbuild`).

The web UI in `www/` is minified (Node, via `bundle/`) and gzipped by
`scripts/build_www.py` into a LittleFS image that is flashed with the firmware.
Without Node installed the files are only gzipped, not minified.

## Flash layout

```
0x009000  nvs        52K  settings, WiFi credentials
0x016000  otadata     8K
0x018000  phy_init    4K
0x020000  app0     1664K  OTA slot A
0x1C0000  app1     1664K  OTA slot B
0x360000  www       256K  LittleFS, web UI (single copy)
0x3A0000  fwstore   320K  sensor firmware images
0x3F0000  coredump   64K
0x400000  fwstore_ext 4M  more sensor images, 8 MB boards only
```

`partitions_4mb.csv` and `partitions_8mb.csv` are identical below 4 MB, so one
firmware and one web UI image serve both. The firmware picks the table by the
flash size in the bootloader header when it migrates a station
(`core/bd_migrate.cpp`): from the Arduino layout, and from the first ESP-IDF
layout (0.7.x - 0.8.x: two 1792K slots, www 320K at 0x3A0000, no image store).
`make_release.py` writes an all-in-one image for each size.

Current images: 1.31 MB on the S3 (79 % of a slot), 1.45 MB on the C6.

A single image for a web flasher can be merged from the build output:

```bash
esptool.py --chip esp32s3 merge_bin -o all-in-one.bin --flash_size 4MB \
  0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin \
  0x16000 build/ota_data_initial.bin 0x20000 build/breezedude_gs.bin 0x3a0000 build/www.bin
```

The `www` partition is deliberately **not** duplicated: the firmware carries an
embedded fallback updater page, so an empty or damaged web UI partition can
always be re-uploaded over HTTP (`POST /update` with `www.bin`) without serial
access. `GET /partition_status` reports what is actually on flash.

## Light sleep

The point of the task rewrite. Every task blocks instead of polling:

- **radio** waits on the SX126x/SX127x DIO1 interrupt, which is also the GPIO
  light-sleep wake source (level triggered - the only kind `gpio_wakeup_enable`
  accepts). No packet is lost while the CPU is asleep.
- **net** blocks on a notification with a one-second ceiling for reconnects.
- **http** blocks in `select()`; WiFi stays associated through DTIM-based modem
  sleep (`WIFI_PS_MAX_MODEM`, `listen_interval = 3`), so the web UI and the APRS
  connection keep working.
- **power** wakes every 5 s for the battery ADC and the sleep schedule.

Sleep is held off while a USB host has the console open
(`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`), during the configurable boot grace
period, and during firmware transfers. `get_info_update` reports the current
state as `sleep_state`.

Expect the deepest sleep with FANET only. With FLARM or ADS-L reception enabled
the scheduler has to hit transmit windows every few seconds, so the CPU wakes
accordingly - `flarm_scheduler_next_delay_ms()` keeps those wake-ups as rare as
the duty profile allows.

## Releasing

1. Raise `CONFIG_APP_PROJECT_VER` (sdkconfig.defaults and sdkconfig) - every
   change, UI-only ones included, gets a new version.
2. `idf.py build`, then `python tools/make_release.py`.
3. Upload `release/<version>/firmware.bin` and `www.bin` to
   `/gs/<channel>/<version>/` and put `manifest-<channel>.json` into the channel.

Stations with auto update on install it without user action, Arduino ones
included (they migrate on first boot). The sequence on a station:

- the firmware is downloaded and installed, the station reboots;
- the new image runs "pending verification" for `CONFIG_BDGS_MARK_VALID_AFTER_S`
  (5 min). A reset before then rolls the bootloader back to the previous image,
  and that version is never installed again on this station;
- once confirmed, the firmware notices its web UI carries another version and
  fetches the matching `www.bin` from the channel, retrying every 5 min if it
  fails. A missing UI (after the migration) is fetched immediately.

`make_release.py` refuses rollback test builds, mismatching firmware/UI
versions and images that cannot update over plain HTTP.

### Alpha channel (internal testing)

Alpha builds carry `CONFIG_BDGS_FW_ALPHA_CHANNEL` (see `sdkconfig.defaults.alpha`)
and only they offer "alpha" in the web UI's update branch:

```bash
idf.py -B build.alpha -D SDKCONFIG=sdkconfig.alpha -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.alpha" build
python tools/make_release.py --build build.alpha     # -> release/<version>-alpha, alpha channel only
```

A test station gets its first alpha build by hand (web upload or the
all-in-one image), "alpha" is selected once in the settings, and from then on it
follows `/gs/alpha/`. A regular build turns a stored "alpha" back into "stable",
and `make_release.py` never packages an alpha build for beta or stable.

## Access point

The SoftAP cannot modem-sleep - on the Heltec V3 it costs ~100 mA (113 mA vs
11-15 mA at 5 V input). Three modes, set in the WiFi section of the UI:

| Mode | AP on |
|---|---|
| AP always on | always |
| Turn off automatically | after every real boot and on a press of the user button; off after `CONFIG_BDGS_AP_IDLE_OFF_MIN` (3) minutes without a client. Losing the uplink does **not** reopen it. Deep-sleep wakes do not open it either. |
| neither (default) | while the STA uplink is down |

The auto-off mode is the one for solar stations: in the default mode a dead
router turns the station into a 100 mA SoftAP until the battery is empty.

## Web UI

Four tabs: **Dashboard** (status, 24 h activity, reception quality, battery
history, weather stations with details, live traffic), **Tools** (sensor OTA
monitor, APRS console), **Settings** and **System** (device facts, firmware
update, backup, system console).

One websocket at `/ws`. Requests are `{"cmd": ...}`:

| cmd | reply |
|---|---|
| `info` | `{"type":"info"}` - version, build, chip, flash, MAC, reset reason |
| `status` | `{"type":"status"}` - WiFi, internet, OGN, Breezedude upload, radio counters, battery; polled every 2 s |
| `stats` | `{"type":"stats"}` - 24 h in 15-min slots, plus 3 days of battery voltage on battery stations |
| `config` | `{"type":"config"}` - all settings including WiFi, then `{"update_channels":[{name,version,description}...]}` from the last manifest |
| `repeat`, `repeat_reset` | `{"type":"repeat","sources":[...],"events":[...]}` - FANET packets by direct / repeated reception since boot |
| `get_fanet` | stored weather, tracking and HW-info packets plus `{"ota_history":[...]}` |
| `save_settings`, `save_wifi`, `wifi_scan`, `check_update`, `reboot`, `set_{ota,flarm,adsl}_verbose` | `{"msg":...}` |

Live data is pushed as `{"weather":[...]}`, `{"tracking":[...]}`,
`{"hwinfo":[...]}`, `{"ota_history":[...]}`, `{"update_channels":[...]}`, `{"repeat":{"event":...,"source":...}}`, `{"ota_progress":{...}}`,
`{"webconsole":...}` and `{"aprsconsole":...}`.

The statistics (`core/bd_stats.cpp`) live in RTC memory: they survive restarts
and the night-time deep sleep, a power cycle clears them. The noise floor is the
channel RSSI sampled every 10 s between packets.

## Layout

```
main/                 app_main + Kconfig (board pins, features, power)
components/compat/    small Arduino compatibility layer (String, Serial,
                      Preferences, WiFiClient, HTTPClient, TimeLib)
components/bdgs/
  app/                boot sequence, task set-up, serial provisioning
  radio/              radio task, FANET/FLARM/ADS-L time sharing, receive paths
  protocols/          FLARM, ADS-L, Manchester decoders (unchanged)
  core/               data model and station stores (unchanged)
  aprs/               OGN uplink (unchanged)
  net/                WiFi, SNTP, captive DNS, outbound HTTP queue
  web/                HTTP server, websocket API, console fan-out
  power/              DFS, light sleep, battery, deep-sleep schedule
  storage/            LittleFS mount for the web UI
  ota/                LoRa OTA gateway, remote config, firmware self update
```

See `PORTING.md` for what was ported verbatim, what was rewritten and the
open points.
