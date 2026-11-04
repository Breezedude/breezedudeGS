// Moves a station from an older flash layout into the current one, forward only.
// The target is partitions_8mb.csv when the bootloader header says 8 MB, else
// partitions_4mb.csv; both are embedded in the image.
//
// Field stations run the Arduino build with this table:
//
//   nvs      0x009000   20K        app0     0x010000  3264K
//   otadata  0x00E000    8K        app1     0x340000  3264K
//   spiffs   0x670000 1536K        coredump 0x7F0000    64K
//
// They receive this firmware as an ordinary OTA image: esp32FOTA writes it into
// the free app slot and the Arduino bootloader (which stays on the chip) boots
// it with the OLD table. This module then moves the station into the table the
// firmware was built for, which is embedded in the image:
//
//   stage A  running from old app0. The new app0, otadata and phy_init all lie
//            inside this very image, so the image first copies itself into old
//            app1 with the normal OTA API and reboots.
//   stage B  running from old app1. Copy the image to the new app0 address,
//            write a fresh otadata that selects it, clear the flash the new NVS
//            grows into, then replace the partition table and reboot. The
//            bootloader comes up with the new table and boots new app0.
//
// Everything up to the table write is idempotent: a power cut simply reruns the
// stage on the next boot. The table write is one 4 KB sector (~50 ms) and the
// only step whose interruption leaves the station unbootable - the same exposure
// the Arduino firmware's own layout migration had. It goes through the public
// OTA API (partition type PARTITION_TABLE), which verifies the new table
// including its MD5 before returning.
//
// The NVS partition starts at the same address in both layouts and only grows,
// so stored settings and WiFi credentials survive. The web UI partition starts
// empty and is fetched or uploaded afterwards (fallback page meanwhile).
//
// The first ESP-IDF builds (up to 0.8.x) used a 4 MB table without an image
// store, with app0 at the same address as now:
//
//   app0 0x020000 1792K   app1 0x1E0000 1792K   www 0x3A0000 320K
//
// A station running from app0 only needs the new table. One running from app1
// first copies itself into app0 and reboots, as in stage A.
#include "bd_migrate.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_flash.h"
#include "esp_flash_partitions.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "spi_flash_mmap.h"

static const char* TAG = "migrate";

// Built from the project's partition CSVs by components/bdgs/CMakeLists.txt.
extern const uint8_t table_4mb_start[] asm("_binary_target_partitions_4mb_bin_start");
extern const uint8_t table_4mb_end[] asm("_binary_target_partitions_4mb_bin_end");
extern const uint8_t table_8mb_start[] asm("_binary_target_partitions_8mb_bin_start");
extern const uint8_t table_8mb_end[] asm("_binary_target_partitions_8mb_bin_end");

// The Arduino layout, matched exactly. Anything else is left alone.
static constexpr uint32_t OLD_APP0 = 0x010000;
static constexpr uint32_t OLD_APP1 = 0x340000;
static constexpr uint32_t OLD_APP_SIZE = 0x330000;
static constexpr uint32_t OLD_FS = 0x670000;
static constexpr uint32_t OLD_FS_SIZE = 0x180000;

// The first ESP-IDF layout.
static constexpr uint32_t IDF1_APP0 = 0x020000;
static constexpr uint32_t IDF1_APP1 = 0x1E0000;
static constexpr uint32_t IDF1_APP_SIZE = 0x1C0000;
static constexpr uint32_t IDF1_WWW = 0x3A0000;
static constexpr uint32_t IDF1_WWW_SIZE = 0x50000;

static constexpr size_t COPY_CHUNK = 4096;
static constexpr uint32_t MAX_ATTEMPTS = 3;

static const char* NVS_NAMESPACE = "bd_migrate";
static const char* NVS_KEY_DONE = "done";

// Survives software resets but not power cycles: stops a station that keeps
// failing a migration step from rebooting forever.
static RTC_NOINIT_ATTR uint32_t s_attempt_magic;
static RTC_NOINIT_ATTR uint32_t s_attempts;
static constexpr uint32_t ATTEMPT_MAGIC = 0xB0EEB0EE;

struct TargetLayout {
  uint32_t app0_offset = 0, app0_size = 0;
  uint32_t otadata_offset = 0, otadata_size = 0;
  uint32_t phy_offset = 0, phy_size = 0;
  uint32_t nvs_offset = 0, nvs_size = 0;
};

static size_t align_up(size_t value, size_t align) { return (value + align - 1) & ~(align - 1); }

// What the flash driver may use is the size in the bootloader header, which can
// be less than the chip has.
static void target_table(const uint8_t** data, size_t* len) {
  uint32_t flash = 0;
  esp_flash_get_size(nullptr, &flash);
  if (flash >= 8 * 1024 * 1024) {
    *data = table_8mb_start;
    *len = (size_t)(table_8mb_end - table_8mb_start);
  } else {
    *data = table_4mb_start;
    *len = (size_t)(table_4mb_end - table_4mb_start);
  }
}

static bool overlaps(uint32_t a, uint32_t a_len, uint32_t b, uint32_t b_len) {
  return a < b + b_len && b < a + a_len;
}

static bool parse_target_table(TargetLayout* out) {
  const uint8_t* table;
  size_t len;
  target_table(&table, &len);
  if (len < sizeof(esp_partition_info_t) || len > ESP_PARTITION_TABLE_MAX_LEN) return false;

  int count = 0;
  if (esp_partition_table_verify((const esp_partition_info_t*)table, false, &count) != ESP_OK) {
    return false;
  }

  const esp_partition_info_t* entries = (const esp_partition_info_t*)table;
  for (int i = 0; i < count; i++) {
    const esp_partition_info_t& e = entries[i];
    if (e.type == PART_TYPE_APP && e.subtype == PART_SUBTYPE_OTA_FLAG) {
      out->app0_offset = e.pos.offset;
      out->app0_size = e.pos.size;
    } else if (e.type == PART_TYPE_DATA && e.subtype == PART_SUBTYPE_DATA_OTA) {
      out->otadata_offset = e.pos.offset;
      out->otadata_size = e.pos.size;
    } else if (e.type == PART_TYPE_DATA && e.subtype == PART_SUBTYPE_DATA_RF) {
      out->phy_offset = e.pos.offset;
      out->phy_size = e.pos.size;
    } else if (e.type == PART_TYPE_DATA && e.subtype == PART_SUBTYPE_DATA_WIFI) {
      out->nvs_offset = e.pos.offset;
      out->nvs_size = e.pos.size;
    }
  }
  return out->app0_size && out->otadata_size == 2 * SPI_FLASH_SEC_SIZE && out->nvs_size;
}

static bool is_arduino_layout() {
  const esp_partition_t* app0 =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  const esp_partition_t* app1 =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  const esp_partition_t* fs =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  const esp_partition_t* www =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "www");

  return app0 && app1 && fs && !www && app0->address == OLD_APP0 && app0->size == OLD_APP_SIZE &&
         app1->address == OLD_APP1 && app1->size == OLD_APP_SIZE && fs->address == OLD_FS &&
         fs->size == OLD_FS_SIZE;
}

static bool is_first_idf_layout() {
  const esp_partition_t* app0 =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  const esp_partition_t* app1 =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  const esp_partition_t* www =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "www");
  const esp_partition_t* store =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "fwstore");

  return app0 && app1 && www && !store && app0->address == IDF1_APP0 && app0->size == IDF1_APP_SIZE &&
         app1->address == IDF1_APP1 && app1->size == IDF1_APP_SIZE && www->address == IDF1_WWW &&
         www->size == IDF1_WWW_SIZE;
}

static size_t running_image_length(const esp_partition_t* running) {
  esp_image_metadata_t meta = {};
  const esp_partition_pos_t pos = {running->address, running->size};
  if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) return 0;
  return meta.image_len;
}

// --- stage A: into the other slot -----------------------------------------------
// Copies the running image into the other OTA slot, which must start at `to`,
// and boots it from there.
static esp_err_t copy_self_to_other_slot(const esp_partition_t* running, size_t image_len, uint32_t to) {
  const esp_partition_t* app1 = esp_ota_get_next_update_partition(nullptr);
  if (!app1 || app1->address != to) return ESP_ERR_NOT_FOUND;

  uint8_t* buf = (uint8_t*)malloc(COPY_CHUNK);
  if (!buf) return ESP_ERR_NO_MEM;

  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(app1, image_len, &handle);
  for (size_t off = 0; err == ESP_OK && off < image_len; off += COPY_CHUNK) {
    size_t n = image_len - off < COPY_CHUNK ? image_len - off : COPY_CHUNK;
    err = esp_partition_read(running, off, buf, n);
    if (err == ESP_OK) err = esp_ota_write(handle, buf, n);
  }
  free(buf);

  if (err != ESP_OK) {
    if (handle) esp_ota_abort(handle);
    return err;
  }
  err = esp_ota_end(handle);  // verifies the copied image
  if (err == ESP_OK) err = esp_ota_set_boot_partition(app1);
  return err;
}

static void note_migration_done() {
  nvs_handle_t nvs;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u8(nvs, NVS_KEY_DONE, 1);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

// --- stage B: old app1 -> new layout ------------------------------------------
// Writes go through the old app0 partition handle: the new app0, otadata,
// phy_init and the NVS growth area all lie inside it, and it is not running.
static esp_err_t write_via_old_app0(const esp_partition_t* old_app0, uint32_t abs_offset,
                                    const void* data, size_t len) {
  return esp_partition_write(old_app0, abs_offset - old_app0->address, data, len);
}

static esp_err_t erase_via_old_app0(const esp_partition_t* old_app0, uint32_t abs_offset,
                                    size_t len) {
  return esp_partition_erase_range(old_app0, abs_offset - old_app0->address,
                                   align_up(len, SPI_FLASH_SEC_SIZE));
}

static esp_err_t prepare_new_layout(const esp_partition_t* running, size_t image_len,
                                    const TargetLayout& t) {
  const esp_partition_t* old_app0 =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  if (!old_app0 || old_app0 == running) return ESP_ERR_INVALID_STATE;

  const uint32_t old_lo = old_app0->address;
  const uint32_t old_hi = old_app0->address + old_app0->size;
  const uint32_t nvs_end = t.nvs_offset + t.nvs_size;
  const bool fits = t.app0_offset >= old_lo && t.app0_offset + t.app0_size <= old_hi &&
                    t.otadata_offset >= old_lo && t.otadata_offset + t.otadata_size <= old_hi &&
                    image_len <= t.app0_size &&
                    !overlaps(t.app0_offset, image_len, running->address, running->size);
  if (!fits) {
    ESP_LOGE(TAG, "target layout does not fit inside the old app0 slot - not migrating");
    return ESP_ERR_INVALID_SIZE;
  }

  uint8_t* buf = (uint8_t*)malloc(COPY_CHUNK);
  if (!buf) return ESP_ERR_NO_MEM;

  // 1. the application image at its new home
  ESP_LOGW(TAG, "copying %u byte image to 0x%06x", (unsigned)image_len, (unsigned)t.app0_offset);
  esp_err_t err = erase_via_old_app0(old_app0, t.app0_offset, image_len);
  for (size_t off = 0; err == ESP_OK && off < image_len; off += COPY_CHUNK) {
    size_t n = image_len - off < COPY_CHUNK ? image_len - off : COPY_CHUNK;
    err = esp_partition_read(running, off, buf, n);
    if (err == ESP_OK) err = write_via_old_app0(old_app0, t.app0_offset + off, buf, n);
  }
  free(buf);
  if (err != ESP_OK) return err;

  esp_image_metadata_t meta = {};
  const esp_partition_pos_t pos = {t.app0_offset, t.app0_size};
  if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK ||
      meta.image_len != image_len) {
    ESP_LOGE(TAG, "copied image does not verify");
    return ESP_ERR_INVALID_CRC;
  }

  // 2. otadata selecting new app0 (sequence 1 -> slot 0), already confirmed so
  //    the bootloader's rollback logic does not treat it as a fresh update
  err = erase_via_old_app0(old_app0, t.otadata_offset, t.otadata_size);
  if (err != ESP_OK) return err;
  esp_ota_select_entry_t select;
  memset(&select, 0xFF, sizeof(select));
  select.ota_seq = 1;
  select.ota_state = ESP_OTA_IMG_VALID;
  select.crc = esp_rom_crc32_le(UINT32_MAX, (const uint8_t*)&select.ota_seq, sizeof(select.ota_seq));
  err = write_via_old_app0(old_app0, t.otadata_offset, &select, sizeof(select));
  if (err != ESP_OK) return err;

  // 3. phy_init and the part of the old app0 slot the larger NVS grows into.
  //    The old otadata sectors in between stay until the table switch - they
  //    are still live - and NVS discards them as foreign pages afterwards.
  if (t.phy_size && t.phy_offset >= old_lo && t.phy_offset + t.phy_size <= old_hi) {
    err = erase_via_old_app0(old_app0, t.phy_offset, t.phy_size);
    if (err != ESP_OK) return err;
  }
  if (nvs_end > old_lo && nvs_end <= t.otadata_offset) {
    err = erase_via_old_app0(old_app0, old_lo, nvs_end - old_lo);
    if (err != ESP_OK) return err;
  }

  // 4. note for the first boot in the new layout
  note_migration_done();
  return ESP_OK;
}

static esp_err_t write_partition_table() {
  const uint8_t* source;
  size_t len;
  target_table(&source, &len);

  const esp_partition_t* table = nullptr;
  esp_err_t err = esp_partition_register_external(
      nullptr, ESP_PRIMARY_PARTITION_TABLE_OFFSET, ESP_PARTITION_TABLE_SIZE, "PrimaryPrtTable",
      ESP_PARTITION_TYPE_PARTITION_TABLE, ESP_PARTITION_SUBTYPE_PARTITION_TABLE_PRIMARY, &table);
  if (err != ESP_OK) return err;

  // The flash driver must not read the source from flash while it writes.
  uint8_t* copy = (uint8_t*)malloc(len);
  if (!copy) return ESP_ERR_NO_MEM;
  memcpy(copy, source, len);

  ESP_LOGW(TAG, "writing the new partition table - do not cut power now");
  esp_ota_handle_t handle = 0;
  err = esp_ota_begin(table, len, &handle);
  if (err == ESP_OK) err = esp_ota_write(handle, copy, len);
  if (err == ESP_OK) {
    err = esp_ota_end(handle);  // verifies the table on flash, MD5 included
  } else if (handle) {
    esp_ota_abort(handle);
  }

  if (err == ESP_OK) {
    uint8_t* readback = (uint8_t*)malloc(len);
    if (!readback || esp_partition_read(table, 0, readback, len) != ESP_OK ||
        memcmp(readback, copy, len) != 0) {
      err = ESP_ERR_INVALID_CRC;
    }
    free(readback);
  }
  free(copy);
  return err;
}

// --- first boot in the new layout ---------------------------------------------
static void finish_after_migration() {
  nvs_handle_t nvs;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
  uint8_t done = 0;
  if (nvs_get_u8(nvs, NVS_KEY_DONE, &done) == ESP_OK && done) {
    // The core dump area still holds bytes of the old app1 slot.
    const esp_partition_t* coredump = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (coredump) esp_partition_erase_range(coredump, 0, coredump->size);
    nvs_erase_key(nvs, NVS_KEY_DONE);
    nvs_commit(nvs);
    ESP_LOGW(TAG, "migration into the new flash layout complete");
  }
  nvs_close(nvs);
}

void bd_migrate_layout_if_needed(void) {
#if CONFIG_BDGS_MIGRATE_ARDUINO_LAYOUT
  const bool arduino = is_arduino_layout();
  const bool first_idf = !arduino && is_first_idf_layout();
  if (!arduino && !first_idf) {
    s_attempt_magic = 0;
    finish_after_migration();
    return;
  }

  const esp_partition_t* running = esp_ota_get_running_partition();
  ESP_LOGW(TAG, "%s flash layout detected, running from %s at 0x%06x", arduino ? "Arduino" : "Old ESP-IDF",
           running->label, (unsigned)running->address);

  // The bootloader has rollback enabled: until this image confirms itself, any
  // reboot sends the station back to the previous firmware - which would
  // download this update again, forever.
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK) ESP_LOGW(TAG, "confirming the image failed: %s", esp_err_to_name(err));

  if (s_attempt_magic != ATTEMPT_MAGIC) {
    s_attempt_magic = ATTEMPT_MAGIC;
    s_attempts = 0;
  }
  if (++s_attempts > MAX_ATTEMPTS) {
    ESP_LOGE(TAG, "migration failed %u times - running in the old layout until power cycled",
             (unsigned)MAX_ATTEMPTS);
    return;
  }

  TargetLayout target;
  if (!parse_target_table(&target)) {
    ESP_LOGE(TAG, "embedded target partition table is invalid - not migrating");
    return;
  }

  const size_t image_len = running_image_length(running);
  if (image_len == 0 || image_len > target.app0_size) {
    ESP_LOGE(TAG, "running image size %u does not fit the new app slot - not migrating", (unsigned)image_len);
    return;
  }

  if (arduino && running->address == OLD_APP0) {
    ESP_LOGW(TAG, "stage A: moving this image to app1 first");
    err = copy_self_to_other_slot(running, image_len, OLD_APP1);
  } else if (arduino && running->address == OLD_APP1) {
    ESP_LOGW(TAG, "stage B: rewriting flash into the new layout");
    err = prepare_new_layout(running, image_len, target);
    if (err == ESP_OK) err = write_partition_table();
  } else if (first_idf && running->address == IDF1_APP1) {
    ESP_LOGW(TAG, "stage A: moving this image to app0 first");
    err = copy_self_to_other_slot(running, image_len, IDF1_APP0);
  } else if (first_idf && running->address == target.app0_offset) {
    // app0, otadata, phy_init and nvs stay where they are; the new www and the
    // image store hold leftovers of the old app1 and www, which LittleFS
    // formats and the store ignores.
    ESP_LOGW(TAG, "rewriting the partition table");
    note_migration_done();
    err = write_partition_table();
  } else {
    ESP_LOGE(TAG, "running from an unexpected slot - not migrating");
    return;
  }

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "migration step failed: %s - continuing in the old layout",
             esp_err_to_name(err));
    return;
  }

  ESP_LOGW(TAG, "migration step done, rebooting");
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
#endif
}
