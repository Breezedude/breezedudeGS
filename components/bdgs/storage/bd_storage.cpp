#include "bd_storage.h"

#include <sys/stat.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char* TAG = "storage";
static bool s_mounted = false;
static uint32_t s_image_crc = 0;

// Every change of the web UI (update, upload, format) remounts the partition,
// so a checksum taken at mount time identifies the installed UI.
static uint32_t partition_crc(void) {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, BD_WWW_PARTITION_LABEL);
  if (!part) return 0;
  static uint8_t buf[1024];
  uint32_t crc = 0;
  for (size_t offset = 0; offset < part->size; offset += sizeof(buf)) {
    if (esp_partition_read(part, offset, buf, sizeof(buf)) != ESP_OK) return 0;
    crc = esp_rom_crc32_le(crc, buf, sizeof(buf));
  }
  return crc;
}

uint32_t bd_storage_image_crc(void) { return s_image_crc; }

bool bd_storage_mount(void) {
  if (s_mounted) return true;

  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = BD_WWW_BASE_PATH;
  conf.partition_label = BD_WWW_PARTITION_LABEL;
  conf.format_if_mount_failed = true;
  conf.dont_mount = false;

  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mounting %s failed: %s", BD_WWW_PARTITION_LABEL, esp_err_to_name(err));
    return false;
  }

  s_mounted = true;
  s_image_crc = partition_crc();
  size_t total = 0, used = 0;
  if (bd_storage_info(&total, &used)) {
    ESP_LOGI(TAG, "www mounted: %u of %u bytes used, web UI %s", (unsigned)used, (unsigned)total,
             bd_storage_has_web_ui() ? "present" : "MISSING (fallback page active)");
  }
  return true;
}

void bd_storage_unmount(void) {
  if (!s_mounted) return;
  esp_vfs_littlefs_unregister(BD_WWW_PARTITION_LABEL);
  s_mounted = false;
}

bool bd_storage_mounted(void) { return s_mounted; }

bool bd_storage_has_web_ui(void) {
  if (!s_mounted) return false;
  struct stat st;
  return stat(BD_WWW_BASE_PATH "/index.html", &st) == 0 ||
         stat(BD_WWW_BASE_PATH "/index.html.gz", &st) == 0;
}

String bd_storage_web_ui_version() {
  if (!s_mounted) return String();
  FILE* file = fopen(BD_WWW_BASE_PATH "/version.txt", "r");
  if (!file) return String();
  char line[32] = {0};
  if (!fgets(line, sizeof(line), file)) line[0] = '\0';
  fclose(file);
  String version(line);
  version.trim();
  return version;
}

bool bd_storage_info(size_t* total_bytes, size_t* used_bytes) {
  if (!s_mounted) return false;
  size_t total = 0, used = 0;
  if (esp_littlefs_info(BD_WWW_PARTITION_LABEL, &total, &used) != ESP_OK) return false;
  if (total_bytes) *total_bytes = total;
  if (used_bytes) *used_bytes = used;
  return true;
}

size_t bd_storage_partition_size(void) {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, BD_WWW_PARTITION_LABEL);
  return part ? part->size : 0;
}

bool bd_storage_format(void) {
  bool was_mounted = s_mounted;
  if (was_mounted) bd_storage_unmount();
  esp_err_t err = esp_littlefs_format(BD_WWW_PARTITION_LABEL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
  }
  if (was_mounted) bd_storage_mount();
  return err == ESP_OK;
}
