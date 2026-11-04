#include "Preferences.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

static const char* TAG = "prefs";

bool Preferences::begin(const char* name, bool read_only) {
  end();
  esp_err_t err = nvs_open(name, read_only ? NVS_READONLY : NVS_READWRITE, &handle_);
  if (err == ESP_ERR_NVS_NOT_FOUND && read_only) {
    // Namespace does not exist yet - behave like Arduino, which reports success
    // and then simply finds no keys.
    handle_ = 0;
    open_ = false;
    read_only_ = true;
    return true;
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "nvs_open(%s) failed: %s", name, esp_err_to_name(err));
    handle_ = 0;
    open_ = false;
    return false;
  }
  open_ = true;
  read_only_ = read_only;
  return true;
}

void Preferences::end() {
  if (open_) {
    nvs_close(handle_);
    open_ = false;
  }
  handle_ = 0;
}

bool Preferences::clear() {
  if (!open_ || read_only_) return false;
  return nvs_erase_all(handle_) == ESP_OK && nvs_commit(handle_) == ESP_OK;
}

bool Preferences::remove(const char* key) {
  if (!open_ || read_only_) return false;
  return nvs_erase_key(handle_, key) == ESP_OK && nvs_commit(handle_) == ESP_OK;
}

size_t Preferences::getBytesLength(const char* key) {
  if (!open_) return 0;
  size_t len = 0;
  if (nvs_get_blob(handle_, key, nullptr, &len) != ESP_OK) return 0;
  return len;
}

bool Preferences::isKey(const char* key) {
  if (!open_) return false;
  size_t len = 0;
  if (nvs_get_blob(handle_, key, nullptr, &len) == ESP_OK) return true;
  size_t str_len = 0;
  if (nvs_get_str(handle_, key, nullptr, &str_len) == ESP_OK) return true;
  uint32_t scratch32 = 0;
  if (nvs_get_u32(handle_, key, &scratch32) == ESP_OK) return true;
  int32_t scratch_i32 = 0;
  if (nvs_get_i32(handle_, key, &scratch_i32) == ESP_OK) return true;
  uint8_t scratch8 = 0;
  return nvs_get_u8(handle_, key, &scratch8) == ESP_OK;
}

size_t Preferences::putBytes(const char* key, const void* value, size_t len) {
  if (!open_ || read_only_ || !value) return 0;
  if (nvs_set_blob(handle_, key, value, len) != ESP_OK) return 0;
  if (nvs_commit(handle_) != ESP_OK) return 0;
  return len;
}

size_t Preferences::getBytes(const char* key, void* buffer, size_t max_len) {
  if (!open_ || !buffer) return 0;
  size_t stored = getBytesLength(key);
  if (stored == 0 || stored > max_len) return 0;
  size_t len = stored;
  if (nvs_get_blob(handle_, key, buffer, &len) != ESP_OK) return 0;
  return len;
}

size_t Preferences::putString(const char* key, const String& value) {
  if (!open_ || read_only_) return 0;
  if (nvs_set_str(handle_, key, value.c_str()) != ESP_OK) return 0;
  if (nvs_commit(handle_) != ESP_OK) return 0;
  return value.length();
}

String Preferences::getString(const char* key, const String& default_value) {
  if (!open_) return default_value;
  size_t len = 0;
  if (nvs_get_str(handle_, key, nullptr, &len) != ESP_OK || len == 0) return default_value;
  String out;
  out.str().resize(len);
  if (nvs_get_str(handle_, key, &out.str()[0], &len) != ESP_OK) return default_value;
  if (len > 0) out.str().resize(len - 1);  // drop the NUL terminator
  return out;
}

size_t Preferences::putUInt(const char* key, uint32_t value) {
  if (!open_ || read_only_) return 0;
  if (nvs_set_u32(handle_, key, value) != ESP_OK) return 0;
  return nvs_commit(handle_) == ESP_OK ? sizeof(value) : 0;
}

uint32_t Preferences::getUInt(const char* key, uint32_t default_value) {
  if (!open_) return default_value;
  uint32_t value = default_value;
  nvs_get_u32(handle_, key, &value);
  return value;
}

size_t Preferences::putInt(const char* key, int32_t value) {
  if (!open_ || read_only_) return 0;
  if (nvs_set_i32(handle_, key, value) != ESP_OK) return 0;
  return nvs_commit(handle_) == ESP_OK ? sizeof(value) : 0;
}

int32_t Preferences::getInt(const char* key, int32_t default_value) {
  if (!open_) return default_value;
  int32_t value = default_value;
  nvs_get_i32(handle_, key, &value);
  return value;
}

size_t Preferences::putBool(const char* key, bool value) { return putUChar(key, value ? 1 : 0); }

bool Preferences::getBool(const char* key, bool default_value) {
  return getUChar(key, default_value ? 1 : 0) != 0;
}

size_t Preferences::putUChar(const char* key, uint8_t value) {
  if (!open_ || read_only_) return 0;
  if (nvs_set_u8(handle_, key, value) != ESP_OK) return 0;
  return nvs_commit(handle_) == ESP_OK ? sizeof(value) : 0;
}

uint8_t Preferences::getUChar(const char* key, uint8_t default_value) {
  if (!open_) return default_value;
  uint8_t value = default_value;
  nvs_get_u8(handle_, key, &value);
  return value;
}

size_t Preferences::putFloat(const char* key, float value) {
  uint32_t raw;
  memcpy(&raw, &value, sizeof(raw));
  return putUInt(key, raw) ? sizeof(value) : 0;
}

float Preferences::getFloat(const char* key, float default_value) {
  uint32_t fallback;
  memcpy(&fallback, &default_value, sizeof(fallback));
  uint32_t raw = getUInt(key, fallback);
  float value;
  memcpy(&value, &raw, sizeof(value));
  return value;
}
