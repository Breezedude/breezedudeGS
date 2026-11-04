// Ground station self-update over HTTP (replaces esp32FOTA).
//
// Manifest format is the one the backend already serves:
//   [{"type":"<channel>","version":"0.7.0","host":"...","port":80,
//     "bin":"/gs/.../firmware.bin","littlefs":"/gs/.../littlefs.bin"}, ...]
//
// The channel name is deliberately NOT the Arduino build's "bd-gs-stable": that
// image expects the old 8 MB partition layout and would try to rewrite the
// partition table of a 4 MB station. See CONFIG_BDGS_FW_CHANNEL_PREFIX.
#pragma once

#include <Arduino.h>

// Checks at most every 6 hours unless `force` is set; installs automatically
// when settings.autoUpdate is on.
void bd_fwupdate_check(bool force);

// Downloads and installs the manifest's firmware image, then reboots.
bool bd_fwupdate_install();

// {"update_channels":[{"name","version","description"}...]} from the last
// manifest fetch; an empty list until the first update check went through.
String bd_fwupdate_channels_json();

// Strips the 5 digit build marker some manifests append to the patch level.
String normalize_online_version(const String& versionRaw);

// Compares dotted versions. <0 means a < b.
int fw_version_compare(const String& a, const String& b);
