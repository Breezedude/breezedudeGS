// Arduino Preferences on top of the ESP-IDF NVS API. Same semantics the ground
// station relies on, in particular getBytes() returning the *stored* length so
// a grown settings struct keeps the defaults of its new fields.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "WString.h"
#include "nvs.h"

class Preferences {
 public:
  ~Preferences() { end(); }

  bool begin(const char* name, bool read_only = false);
  void end();

  bool clear();
  bool remove(const char* key);
  bool isKey(const char* key);

  size_t putBytes(const char* key, const void* value, size_t len);
  size_t getBytes(const char* key, void* buffer, size_t max_len);
  size_t getBytesLength(const char* key);

  size_t putString(const char* key, const String& value);
  String getString(const char* key, const String& default_value = String());

  size_t putUInt(const char* key, uint32_t value);
  uint32_t getUInt(const char* key, uint32_t default_value = 0);
  size_t putInt(const char* key, int32_t value);
  int32_t getInt(const char* key, int32_t default_value = 0);
  size_t putBool(const char* key, bool value);
  bool getBool(const char* key, bool default_value = false);
  size_t putUChar(const char* key, uint8_t value);
  uint8_t getUChar(const char* key, uint8_t default_value = 0);
  size_t putFloat(const char* key, float value);
  float getFloat(const char* key, float default_value = 0.0f);

 private:
  nvs_handle_t handle_ = 0;
  bool open_ = false;
  bool read_only_ = false;
};
