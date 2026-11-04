// Images lie in the store partitions ("fwstore", and "fwstore_ext" on 8 MB
// boards) one after another, each starting on a flash sector with a header:
//
//   +0    FwHeader (magic, size, SHA-256, name, CRC of the header)
//   +256  image data
//
// The header is written last, after the data has been checked, so an
// interrupted download leaves sectors without a valid header - free space.
// Sectors that never held an image (leftovers of an older flash layout) look
// the same. When the space is needed, the image unused the longest goes; when
// it was last used is kept in NVS, so the header never has to be rewritten.
#include "bd_fwstore.h"

#include <string.h>

#include <vector>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

static const char* TAG = "fwstore";

static constexpr uint32_t kSector = 4096;
static constexpr uint32_t kDataOffset = 256;
static constexpr uint32_t kMagic = 0x57464442;  // "BDFW"
static constexpr uint16_t kVersion = 1;
static const char* kLabels[] = {"fwstore", "fwstore_ext"};
static const char* kNvsNamespace = "fwstore";

struct FwHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t dataOffset;
  uint32_t imageSize;
  uint8_t sha256[32];
  char name[48];
  uint32_t crc;  // over everything above
};
static_assert(sizeof(FwHeader) <= kDataOffset, "header must fit before the data");

struct Entry {
  const esp_partition_t* part;
  uint32_t sector;
  uint32_t sectors;
  FwHeader header;
};

static uint32_t header_crc(const FwHeader& h) {
  return esp_rom_crc32_le(0, (const uint8_t*)&h, offsetof(FwHeader, crc));
}

static uint32_t sectors_for(uint32_t imageSize) { return (kDataOffset + imageSize + kSector - 1) / kSector; }

static std::vector<const esp_partition_t*> regions() {
  std::vector<const esp_partition_t*> out;
  for (const char* label : kLabels) {
    const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (p) out.push_back(p);
  }
  return out;
}

static std::vector<Entry> scan() {
  std::vector<Entry> out;
  for (const esp_partition_t* part : regions()) {
    const uint32_t count = part->size / kSector;
    for (uint32_t sector = 0; sector < count;) {
      FwHeader h;
      if (esp_partition_read(part, sector * kSector, &h, sizeof(h)) == ESP_OK && h.magic == kMagic &&
          h.version == kVersion && h.dataOffset == kDataOffset && h.crc == header_crc(h) &&
          sector + sectors_for(h.imageSize) <= count) {
        out.push_back({part, sector, sectors_for(h.imageSize), h});
        sector += sectors_for(h.imageSize);
      } else {
        sector++;
      }
    }
  }
  return out;
}

// --- when an image was last used ------------------------------------------------
static void use_key(const esp_partition_t* part, uint32_t sector, char* key) {
  snprintf(key, 16, "u%06lx", (unsigned long)(part->address + sector * kSector));
}

static uint32_t last_use(const Entry& e) {
  char key[16];
  use_key(e.part, e.sector, key);
  uint32_t value = 0;
  nvs_handle_t nvs;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) == ESP_OK) {
    nvs_get_u32(nvs, key, &value);
    nvs_close(nvs);
  }
  return value;
}

static void mark_used(const esp_partition_t* part, uint32_t sector) {
  char key[16];
  use_key(part, sector, key);
  nvs_handle_t nvs;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) return;
  uint32_t counter = 0;
  nvs_get_u32(nvs, "counter", &counter);
  counter++;
  nvs_set_u32(nvs, "counter", counter);
  nvs_set_u32(nvs, key, counter);
  nvs_commit(nvs);
  nvs_close(nvs);
}

// Clearing the magic is a write of zero bits, which needs no erase.
static void drop(const Entry& e) {
  ESP_LOGI(TAG, "dropping %s (%u bytes)", e.header.name, (unsigned)e.header.imageSize);
  const uint32_t zero = 0;
  esp_partition_write(e.part, e.sector * kSector, &zero, sizeof(zero));
  char key[16];
  use_key(e.part, e.sector, key);
  nvs_handle_t nvs;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_erase_key(nvs, key);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

static bool image_sha256(const esp_partition_t* part, uint32_t offset, uint32_t size, uint8_t out[32]) {
  uint8_t buf[512];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  bool ok = mbedtls_sha256_starts(&ctx, 0) == 0;
  for (uint32_t done = 0; ok && done < size;) {
    const uint32_t n = size - done < sizeof(buf) ? size - done : sizeof(buf);
    ok = esp_partition_read(part, offset + done, buf, n) == ESP_OK && mbedtls_sha256_update(&ctx, buf, n) == 0;
    done += n;
  }
  ok = ok && mbedtls_sha256_finish(&ctx, out) == 0;
  mbedtls_sha256_free(&ctx);
  return ok;
}

bool bd_fwstore_find(const uint8_t sha256[32], uint32_t size, BdFwImage* out) {
  for (const Entry& e : scan()) {
    if (e.header.imageSize != size || memcmp(e.header.sha256, sha256, 32) != 0) continue;
    uint8_t actual[32];
    const uint32_t offset = e.sector * kSector + kDataOffset;
    if (!image_sha256(e.part, offset, size, actual) || memcmp(actual, sha256, 32) != 0) {
      ESP_LOGW(TAG, "%s no longer matches its checksum", e.header.name);
      drop(e);
      continue;
    }
    mark_used(e.part, e.sector);
    out->part = e.part;
    out->offset = offset;
    out->size = size;
    return true;
  }
  return false;
}

// The first run of free sectors long enough, after the entries in `taken`.
static bool find_free_run(const std::vector<Entry>& taken, uint32_t need, const esp_partition_t** part,
                          uint32_t* sector) {
  for (const esp_partition_t* p : regions()) {
    const uint32_t count = p->size / kSector;
    std::vector<bool> used(count, false);
    for (const Entry& e : taken) {
      if (e.part != p) continue;
      for (uint32_t i = 0; i < e.sectors; i++) used[e.sector + i] = true;
    }
    uint32_t run = 0;
    for (uint32_t i = 0; i < count; i++) {
      run = used[i] ? 0 : run + 1;
      if (run == need) {
        *part = p;
        *sector = i + 1 - need;
        return true;
      }
    }
  }
  return false;
}

bool bd_fwstore_begin(uint32_t size, BdFwWriter* w) {
  *w = {};
  const uint32_t need = sectors_for(size);
  std::vector<Entry> entries = scan();
  const esp_partition_t* part = nullptr;
  uint32_t sector = 0;
  while (!find_free_run(entries, need, &part, &sector)) {
    if (entries.empty()) {
      ESP_LOGW(TAG, "no room for %u bytes", (unsigned)size);
      return false;
    }
    size_t oldest = 0;
    uint32_t oldestUse = UINT32_MAX;
    for (size_t i = 0; i < entries.size(); i++) {
      const uint32_t use = last_use(entries[i]);
      if (use < oldestUse) {
        oldest = i;
        oldestUse = use;
      }
    }
    drop(entries[oldest]);
    entries.erase(entries.begin() + oldest);
  }

  if (esp_partition_erase_range(part, sector * kSector, need * kSector) != ESP_OK) return false;
  w->image.part = part;
  w->image.offset = sector * kSector + kDataOffset;
  w->image.size = size;
  w->ok = true;
  return true;
}

bool bd_fwstore_write(BdFwWriter* w, const uint8_t* data, size_t len) {
  if (!w->ok || w->written + len > w->image.size) return w->ok = false;
  if (esp_partition_write(w->image.part, w->image.offset + w->written, data, len) != ESP_OK) return w->ok = false;
  w->written += len;
  return true;
}

bool bd_fwstore_finish(BdFwWriter* w, const uint8_t sha256[32], const char* name, BdFwImage* out) {
  if (!w->ok || w->written != w->image.size) return false;
  uint8_t actual[32];
  if (!image_sha256(w->image.part, w->image.offset, w->image.size, actual) || memcmp(actual, sha256, 32) != 0) {
    ESP_LOGW(TAG, "%s does not match its checksum, not kept", name);
    return false;
  }

  FwHeader h;
  memset(&h, 0, sizeof(h));
  h.magic = kMagic;
  h.version = kVersion;
  h.dataOffset = kDataOffset;
  h.imageSize = w->image.size;
  memcpy(h.sha256, sha256, 32);
  strlcpy(h.name, name, sizeof(h.name));
  h.crc = header_crc(h);
  const uint32_t headerAt = w->image.offset - kDataOffset;
  FwHeader check;
  if (esp_partition_write(w->image.part, headerAt, &h, sizeof(h)) != ESP_OK ||
      esp_partition_read(w->image.part, headerAt, &check, sizeof(check)) != ESP_OK ||
      memcmp(&h, &check, sizeof(h)) != 0) {
    return false;
  }
  mark_used(w->image.part, headerAt / kSector);
  ESP_LOGI(TAG, "kept %s (%u bytes) in %s", name, (unsigned)h.imageSize, w->image.part->label);
  *out = w->image;
  return true;
}

bool bd_fwstore_read(const BdFwImage& image, uint32_t offset, void* buf, size_t len) {
  if (!image.part || offset + len > image.size) return false;
  return esp_partition_read(image.part, image.offset + offset, buf, len) == ESP_OK;
}

void bd_fwstore_space(uint32_t* total, uint32_t* free) {
  *total = 0;
  uint32_t used = 0;
  for (const esp_partition_t* p : regions()) *total += p->size;
  for (const Entry& e : scan()) used += e.sectors * kSector;
  *free = *total - used;
}
