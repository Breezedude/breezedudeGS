// LittleFS partition holding the web UI.
//
// There is exactly one copy (no A/B pair): the firmware ships an embedded
// fallback updater page, so an empty or outdated "www" partition can always be
// re-flashed from the browser without serial access.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BD_WWW_BASE_PATH "/www"
#define BD_WWW_PARTITION_LABEL "www"

#ifdef __cplusplus
extern "C" {
#endif

bool bd_storage_mount(void);
void bd_storage_unmount(void);
bool bd_storage_mounted(void);

// True when a usable web UI is present (index.html or its gzipped twin).
bool bd_storage_has_web_ui(void);

bool bd_storage_info(size_t* total_bytes, size_t* used_bytes);
size_t bd_storage_partition_size(void);
bool bd_storage_format(void);

// Checksum of the partition as mounted; changes with every new web UI.
uint32_t bd_storage_image_crc(void);

#ifdef __cplusplus
}

#include <Arduino.h>

// Firmware version the installed web UI was built for (version.txt), or an
// empty string when the UI is missing or predates the marker.
String bd_storage_web_ui_version();
#endif
