#!/usr/bin/env python3
"""Package a build for the update channels.

Run after `idf.py build`:

    python tools/make_release.py                 # defaults below
    python tools/make_release.py --host fanet.breezedude.de --base /gs

Creates release/<version>/ with

    firmware.bin      application image   -> "bin"  of the manifest entry
    www.bin           web UI image        -> "www"  of the manifest entry
    all-in-one.bin    full 4 MB image for a fresh USB install (offset 0x0)
    all-in-one-8mb.bin  the same for boards with 8 MB, with the larger table
    SHA256SUMS.txt
    manifest-<channel>.json   one entry per channel: stable + beta for a regular
                              build, alpha only for a CONFIG_BDGS_FW_ALPHA_CHANNEL build

Upload firmware.bin and www.bin to <base>/<channel>/<version>/ on the server and
put the entry into that channel's manifest. Stations - including ones still on
the Arduino firmware - install it without user action if auto update is on.

The entry deliberately has no "littlefs" key: esp32FOTA on Arduino stations
flashes that into their old filesystem *before* the application, which would be
wasted at best and leave a broken UI if the app download then failed.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGULAR_CHANNELS = ("stable", "beta")
ALPHA_CHANNELS = ("alpha",)


def fail(message: str) -> None:
    print("make_release: " + message, file=sys.stderr)
    sys.exit(1)


def load_json(path: str) -> dict:
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default=os.path.join(PROJECT, "build"), help="IDF build directory")
    parser.add_argument("--host", default="fanet.breezedude.de")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--base", default="/gs", help="server path prefix; files go to <base>/<channel>/<version>/")
    parser.add_argument("--prefix", default=None, help="channel prefix (default: from sdkconfig)")
    args = parser.parse_args()

    build = os.path.abspath(args.build)
    sdk = load_json(os.path.join(build, "config", "sdkconfig.json"))
    desc = load_json(os.path.join(build, "project_description.json"))
    version = desc.get("project_version", "")
    if not version:
        fail("no project_version in project_description.json - run idf.py build first")

    # Refuse anything that must never reach a station.
    if int(sdk.get("BDGS_TEST_CRASH_AFTER_S", 0)) != 0:
        fail("this is a rollback TEST build (BDGS_TEST_CRASH_AFTER_S != 0) - not releasing it")
    # Manifest entries point to plain HTTP (host + port); without this option
    # esp_https_ota refuses to start and stations could never update again.
    if not sdk.get("ESP_HTTPS_OTA_ALLOW_HTTP", False):
        fail("CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP is off - this image could not self-update over HTTP")
    www_version_file = os.path.join(build, "wwwdata", "version.txt")
    if not os.path.isfile(www_version_file):
        fail("build/wwwdata/version.txt missing - web UI was built without a version")
    with open(www_version_file, encoding="ascii") as handle:
        www_version = handle.read().strip()
    if www_version != version:
        fail(f"web UI is for {www_version}, firmware is {version} - rebuild")

    # flasher_args.json is what `idf.py flash` itself uses: every image with its
    # offset plus the flash settings, so the merged image matches a USB flash.
    flasher = load_json(os.path.join(build, "flasher_args.json"))
    images = {offset: os.path.join(build, rel) for offset, rel in flasher["flash_files"].items()}
    for offset, path in images.items():
        if not os.path.isfile(path):
            fail(f"image for {offset} not found at {path}")
    app_bin = os.path.join(build, desc["app_bin"])
    www_bin = os.path.join(build, "www.bin")
    if www_bin not in images.values():
        fail("www.bin is not part of the flash layout - check littlefs_create_partition_image")

    # Alpha builds get their own folder: the same version number is usually
    # rebuilt without the alpha option for beta/stable later.
    alpha_build = bool(sdk.get("BDGS_FW_ALPHA_CHANNEL", False))
    out = os.path.join(PROJECT, "release", version + ("-alpha" if alpha_build else ""))
    os.makedirs(out, exist_ok=True)
    shutil.copyfile(app_bin, os.path.join(out, "firmware.bin"))
    shutil.copyfile(www_bin, os.path.join(out, "www.bin"))

    chip = flasher["extra_esptool_args"]["chip"]
    settings = flasher["flash_settings"]
    # The 8 MB image differs in its partition table and in the flash size in the
    # bootloader header, which is what the firmware goes by to use the second
    # 4 MB (partitions_8mb.csv).
    table_8mb = os.path.join(build, "esp-idf", "bdgs", "target_partitions_8mb.bin")
    if not os.path.isfile(table_8mb):
        fail(f"8 MB partition table not found at {table_8mb}")
    table_offset = hex(int(sdk.get("PARTITION_TABLE_OFFSET", 0x8000)))
    for name, flash_size, table in (("all-in-one.bin", settings["flash_size"], None),
                                    ("all-in-one-8mb.bin", "8MB", table_8mb)):
        merge = [sys.executable, "-m", "esptool", "--chip", chip, "merge_bin",
                 "-o", os.path.join(out, name),
                 "--flash_mode", settings["flash_mode"], "--flash_freq", settings["flash_freq"],
                 "--flash_size", flash_size]
        for offset in sorted(images, key=lambda o: int(o, 16)):
            image = table if table and int(offset, 16) == int(table_offset, 16) else images[offset]
            merge += [offset, image]
        subprocess.run(merge, check=True, stdout=subprocess.DEVNULL)

    prefix = args.prefix or sdk.get("BDGS_FW_CHANNEL_PREFIX", "bd-gs")
    # An alpha build offers the alpha channel in its UI, so it must never reach
    # stations on beta or stable.
    channels = ALPHA_CHANNELS if alpha_build else REGULAR_CHANNELS
    for channel in channels:
        path = f"{args.base.rstrip('/')}/{channel}/{version}"
        entry = {
            "type": f"{prefix}-{channel}",
            "version": version,
            "host": args.host,
            "port": args.port,
            "bin": f"{path}/firmware.bin",
            "www": f"{path}/www.bin",
        }
        with open(os.path.join(out, f"manifest-{channel}.json"), "w", encoding="utf-8") as handle:
            json.dump(entry, handle, indent=2)
            handle.write("\n")

    sums = []
    for name in ("firmware.bin", "www.bin", "all-in-one.bin", "all-in-one-8mb.bin"):
        sums.append(f"{sha256(os.path.join(out, name))}  {name}")
    with open(os.path.join(out, "SHA256SUMS.txt"), "w", encoding="ascii") as handle:
        handle.write("\n".join(sums) + "\n")

    print(f"release {version} for {chip}, manifest URL in the image: {sdk.get('BDGS_FW_MANIFEST_URL')}")
    print(f"  channels: {', '.join(channels)}")
    print(f"  {out}")
    for name in sorted(os.listdir(out)):
        print(f"    {name:20} {os.path.getsize(os.path.join(out, name)):>9} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
