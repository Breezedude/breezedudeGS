# Porting notes

Reference: `../breezedudeGS` at commit `d7b8ceb` plus its uncommitted FLARM/ADS-L
work (`src/flarm.cpp`, `src/adsl.cpp`, `src/manchester.cpp`).

## Taken over unchanged

| File | now at |
|---|---|
| `types.h` / `types.cpp` | `components/bdgs/include`, `core/` |
| `flarm.*`, `adsl.*`, `manchester.*` | `include/`, `protocols/` |
| `aprs.*` | `include/`, `aprs/` |
| `ota_gs.cpp`, `config_gs.*` | `include/`, `ota/` |
| `config_template.h` | `include/` |
| `www/` | `www/` |

Three edits were needed in otherwise untouched files:

1. `ota_gs.cpp` and `config_gs.cpp`: `extern volatile bool receivedFlag;` became
   a comment. The flag is now a `RadioIrqFlag` object declared in `helper.h` -
   see *Interrupt handling* below. Everything those files do with it
   (`if (receivedFlag)`, `receivedFlag = false`) still compiles and behaves the
   same.
2. `types.cpp`: four debug `printf` format specifiers were genuinely wrong
   (`%ld` for an `int` bitfield, `%d` for a `float`). ESP-IDF builds with
   `-Werror=format`, so they are now cast/corrected. Output only.
3. `helper.h` was rewritten rather than copied: the radio API is the same, the
   LittleFS partition lookup now finds the `www` label, and the interrupt flag
   type changed.

## Rewritten against ESP-IDF

| Arduino | replacement |
|---|---|
| `main.cpp` `setup()`/`loop()` | `app/bd_app.cpp` - boot order and tasks |
| radio part of `main.cpp` | `radio/bd_radio.cpp` - task, scheduler, receive paths |
| `ws.h` | `web/bd_ws_api.cpp` (commands, JSON packers) + `net/bd_net.cpp` (scan, forward queue, internet watchdog) |
| `recovery.cpp` | `web/bd_http.cpp` - upload, `/partition_status`, fallback page |
| console helpers in `main.cpp` | `web/bd_console.cpp` |
| `power_management.cpp` | `power/bd_power.cpp` + `esp_pm` |
| `esp32FOTA` | `ota/bd_fwupdate.cpp` |
| `Preferences`, `WiFiClient`, `HTTPClient`, `TimeLib`, `String`, `Serial` | `components/compat/` |
| `DNSServer` | `dns_task` in `net/bd_net.cpp` |
| `LittleFS` (Arduino) | `joltwallet/littlefs` via `storage/bd_storage.cpp` |

The websocket JSON contract is unchanged, including field names and the
`webconsole` / `aprsconsole` / `ota_progress` / `webconsole_history` messages, so
`www/app.js` needed no changes. One field was added: `sleep_state` in
`get_info_update` (unknown keys are ignored by the UI).

## Interrupt handling

GPIO wake-up from light sleep only works with level triggers
(`gpio_wakeup_enable()` rejects edges). A level-triggered ISR would re-enter
forever, so:

- the ISR masks the interrupt, sets the flag and notifies the radio task;
- clearing the flag (`receivedFlag = false`, which every receive path does before
  reading the packet out) marks it for re-arming;
- the next read of the flag, or the radio task before it blocks, re-arms the
  interrupt and re-checks the pin level.

That keeps `ota_gs.cpp`'s own polling loops working while the same pin wakes the
chip from sleep.

## Concurrency

The Arduino build was single threaded. Added locks:

- `store_lock()` around `weatherStore` / `trackingStore` / `hwInfoStore`
  (radio task writes, web task reads).
- `settings_lock()` around settings writes.
- `aprs_lock()` around every `Aprs` call - the APRS task runs `aprs.run()` while
  the radio task calls `sendTrackingData()` / `sendWeatherData()`, which share
  one socket.
- `bd_radio_lock()` for exclusive radio access; the LoRa OTA gateway holds it for
  a whole transfer, which is why its blocking loops are safe.
- Websocket writes are funnelled through `httpd_queue_work()`, so only the HTTP
  server task touches the sockets.

## Rollout to Arduino stations

This firmware ships through the same update channels as the Arduino build
(`bd-gs-stable`, `bd-gs-beta`) and migrates the flash on first boot,
`core/bd_migrate.cpp`. Forward only: a station installs an image only if its
version is higher, so an Arduino entry left in a channel is never installed on a
migrated station as long as this firmware's version stays above all Arduino
releases (verified: a station on 0.7.0 reads `bd-gs-stable` = 0.6.5 and keeps
running).

Manifest entry for the rollout:

```json
{"type": "bd-gs-stable", "version": "0.7.0", "host": "fanet.breezedude.de", "port": 80,
 "bin": "/gs/stable/0.7.0/firmware.bin", "www": "/gs/stable/0.7.0/www.bin"}
```

- `bin` is `build/breezedude_gs.bin`, `www` is `build/www.bin`.
- **No `littlefs` key** in this entry: esp32FOTA on the Arduino stations would
  write it into their old filesystem. It ignores `www`; this firmware prefers
  `www` and only falls back to `littlefs`, and only if the image fits its 320 KB
  partition.

The backend (`breezedude_web/backend/breezedude-fanet/routes_ota.js`) builds
these entries from the folders under `firmware/gs/<channel>/<version>/`: a folder
with `www.bin` yields a `www` entry, one with `littlefs.bin` the Arduino-style
`littlefs` entry. Channels are stable, beta and alpha; per channel the newest
*complete* folder wins, so a half-finished upload is skipped. Upload into
`<version>.tmp` and rename when done - the backend ignores folders that are not
plain `x.y.z`.

What happens on a station:

1. esp32FOTA installs the image into the free app slot of the 8 MB table and
   reboots. The Arduino bootloader (IDF 4.4.7, rollback enabled) stays and boots
   it with the old table.
2. The image confirms itself immediately - otherwise the next reboot would roll
   back to Arduino, which would download the update again, forever.
3. Stage A, only when running from old app0: the new app0/otadata/phy_init all
   lie inside the running image, so it copies itself to old app1 and reboots.
4. Stage B, from old app1: copy the image to the new app0 address, write an
   otadata entry selecting it, clear the area the larger NVS grows into, then
   write the new partition table through the OTA API (type `PARTITION_TABLE`,
   verified including MD5) and reboot.
5. First boot in the new layout: the core dump area is cleared, the empty `www`
   partition is formatted, and the next update check installs the `www` image.

Settings survive: NVS starts at 0x9000 in both layouts and only grows. Every
step before the table write is idempotent; a power cut during the ~50 ms table
write (one 4 KB sector) leaves the station unbootable, the same exposure the
Arduino firmware's own GXAirCom migration had. Three failed attempts in a row
stop the migration until the next power cycle; the station then keeps running
in the old layout with the fallback page.

The partition table written is embedded at build time from the configured CSV
and is byte-identical to IDF's own `partition-table.bin`.

## Settings struct

The stored blob is `Settings` verbatim. New fields are appended; the Arduino
layout ends at 376 bytes including two padding bytes, which are zero in every
blob the Arduino firmware wrote (checked against a real field blob), so
`apAutoOff` at offset 374 loads as "off". `reserved[65]` brings the struct to
440 bytes without tail padding; future fields are carved out of it so the size
stays fixed, and must treat 0 as their default. `static_assert`s in `types.h`
pin the layout.

## Deliberately dropped

- The Arduino build's GXAirCom-to-Breezedude table migration
  (`target_partitions_bin.h`): replaced by `bd_migrate.cpp`, which handles the
  Breezedude 8 MB layout only.
- `SPIFFS`-subtype trick for the filesystem partition - the partition is a proper
  `littlefs` subtype now.

## Verified on hardware

Heltec WiFi LoRa 32 V3 (ESP32-S3, 8 MB flash, CP2102 on COM4), 2026-10-20:

- 4 MB partition table accepted on the 8 MB part; the bootloader notes the
  difference and uses the image header's 4 MB.
- **NVS survived the layout change.** The old table had `nvs` at 0x9000 with
  20 KB, the new one starts at the same offset with 52 KB - growing an NVS
  partition is supported, and the station came up with its stored device name,
  position and WiFi credentials.
- LittleFS web UI partition mounted (140 KB of 320 KB used), the UI loads and
  serves gzipped assets with the correct sizes.
- Websocket API answers `get_info`, `get_info_update`, `get_settings`,
  `get_wifi`, `get_fanet`, `save_settings`, `save_wifi`, `wifi_scan`,
  `set_*_verbose`, unknown commands, and replays the console history on connect.
- `POST /update` with `www.bin`: the multipart parser wrote the image so that the
  gzipped `app.js` served afterwards is **byte-identical** (SHA-256) to the local
  build artefact.
- Captive portal probes (`/generate_204`, `/ncsi.txt`, unknown paths) answer 302
  to the AP address, same as the Arduino build.
- SX1262 detected, radio task running, automatic light sleep active after the
  boot grace period, and the FSK scheduler kept reconfiguring the modem for
  minutes **without a single SPI or begin() error while sleeping**.
- Timekeeping across light sleep is correct (measured 570 s of wall clock against
  9 min of reported uptime).
- Modem switch cost: `flarm_switch_us` max 40.9 ms, inside the 60 ms guard band.

2026-10-21, same board on a PPK2 (5 V input):

- **Packet reception during light sleep confirmed** (~1 packet / 22 s in daylight).
- **Firmware update over WiFi** (`POST /update`, firmware.bin) switches the OTA
  slot, and the rollback marking holds across a reset.
- **ISR crash fixed**: RadioLib installs the GPIO ISR service with
  `ESP_INTR_FLAG_IRAM`, so the DIO1 handler also runs while the flash cache is
  off; it called `gpio_intr_disable()` from flash and panicked whenever a packet
  arrived during a flash write (OTA, NVS, LittleFS). Fixed with
  `CONFIG_GPIO_CTRL_FUNC_IN_IRAM`; proven by 37 back-to-back `www.bin` uploads
  over 150 s with 6 packets arriving, no reset, web UI byte-identical afterwards.
- Current at 5 V input: STA + SoftAP 113 mA (an AP cannot modem-sleep), STA only
  with a websocket client 39 mA, STA only idle 31 mA, floor 17 mA (SX1262 in RX
  plus board overhead - LDO, LEDs, CP2102). Measured before the listen-interval
  and internet-check changes below.

2026-10-21, migration from the Arduino firmware, real field state (Arduino 0.6.8
on IDF 4.4.7 with its bootloader, the station's own Arduino-written NVS),
image delivered through the Arduino firmware's `/update` (same Update.h write
path as esp32FOTA):

- **Stage B only** (Arduino running from app0, image lands in app1): copy,
  table switch, boot from new app0 - settings, WiFi and `apAutoOff=false` from
  the Arduino blob's zero padding all intact.
- **Stage A + B** (Arduino first updated itself into app1, image lands in app0):
  self-copy to app1, then stage B. ~21 s and three reboots from upload to WiFi;
  the Arduino bootloader's rollback never triggered.
- A normal follow-up OTA on the migrated station (Arduino bootloader, new table)
  boots from the new app1.
- **Bug found and fixed**: the web UI refill after the migration overflowed the
  `net` task's 5 KB stack (HTTP client + manifest JSON + a 1 KB stack buffer) and
  sent the station into a reboot loop 11 s after every boot. The LoRa OTA gateway
  has the same profile and runs in the radio task. Both tasks now have 8 KB, the
  buffer moved to the heap, and the power task logs every task's stack headroom
  after boot and hourly (`net` keeps ~4 KB free after an update check).

2026-10-24, unattended rollout end to end, against a local copy of the update
backend (same manifest format, plain HTTP):

- Arduino 0.6.8 -> 0.7.90 via the Arduino updater: migration, then the empty
  web UI fetched from the channel's `www`.
- 0.7.90 -> 0.7.91 by auto update; 0.7.91 held its UI back until confirmed, then
  fetched the matching one.
- 0.7.91 -> 0.7.92 (built to abort after 30 s): installed, crashed, the Arduino
  bootloader rolled back to 0.7.91, which then logged that 0.7.92 was rolled back
  and does not install it again. The 0.7.91 UI stayed in place.
- Release 0.7.1 flashed as a fresh install (all-in-one, IDF bootloader) checks
  the real channel, sees Arduino 0.6.5 and stays put.

Bugs the rollout test found and fixed:

- `esp_https_ota` refuses plain HTTP unless `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP`;
  without it a migrated station could never update again.
- With `listen_interval = 10` the AP buffered every frame until the next wake-up:
  1.3 MB took ~105 s. WiFi power save is off during transfers now (~16 s).
- The UART ran off APB, which DFS moves between 80 and 40 MHz; output bypassing
  the driver's PM lock arrived garbled. It runs off XTAL now.

### Open / not reproducible on the bench
- `flarm_entry_late_ms_max` read 304 ms. The metric never resets and its first
  sample is taken whenever FLARM is switched on, which by construction can be up
  to 450 ms late, so a single reading says little - watch whether it grows.
- `wifi_scan` returns only the currently connected AP. The scan runs while the
  station is in AP+STA mode, where esp_wifi restricts scanning; the Arduino build
  used the same mode, so this is probably not a regression, but it was not
  compared side by side.
- Sleep current draw (no meter on the bench) and the C6 pin map (placeholders).
- `gpio_install_isr_service() already installed` appears twice at boot. That is
  RadioLib's own HAL probing two module types; harmless.
