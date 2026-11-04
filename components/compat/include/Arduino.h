// Arduino compatibility surface for the ESP-IDF port.
//
// This is deliberately small: it exists so that the protocol modules
// (FANET/FLARM/ADS-L decoding, APRS, LoRa OTA) stay byte-for-byte comparable
// with the Arduino version of the ground station firmware. Everything that is
// specific to this firmware (WiFi handling, web server, storage, power) is
// written against the ESP-IDF APIs directly and does not use this header.
#pragma once

#include <cmath>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <type_traits>

#include "esp_log.h"

#include "HardwareSerial.h"
#include "IPAddress.h"
#include "Print.h"
#include "WString.h"

typedef uint8_t byte;

// --- Arduino math constants -------------------------------------------------
#ifndef PI
#define PI 3.1415926535897932384626433832795
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define EULER 2.718281828459045235360287471352
#endif

// --- pin / level constants --------------------------------------------------
#define LOW 0
#define HIGH 1
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#define INPUT_PULLDOWN 0x09

// --- arduino-esp32 style logging, mapped onto ESP_LOGx ---------------------
#define log_e(format, ...) ESP_LOGE("arduino", format, ##__VA_ARGS__)
#define log_w(format, ...) ESP_LOGW("arduino", format, ##__VA_ARGS__)
#define log_i(format, ...) ESP_LOGI("arduino", format, ##__VA_ARGS__)
#define log_d(format, ...) ESP_LOGD("arduino", format, ##__VA_ARGS__)
#define log_v(format, ...) ESP_LOGV("arduino", format, ##__VA_ARGS__)

// --- attributes / PROGMEM no-ops -------------------------------------------
#define F(string_literal) (string_literal)
#define PSTR(s) (s)
#define PROGMEM
#ifndef ICACHE_RAM_ATTR
#define ICACHE_RAM_ATTR IRAM_ATTR
#endif

// --- timing -----------------------------------------------------------------
uint32_t millis();
uint64_t micros();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void yield();

// --- GPIO / ADC -------------------------------------------------------------
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t level);
int digitalRead(uint8_t pin);
uint16_t analogRead(uint8_t pin);
uint32_t analogReadMilliVolts(uint8_t pin);

// --- CPU / chip -------------------------------------------------------------
bool setCpuFrequencyMhz(uint32_t mhz);
uint32_t getCpuFrequencyMhz();

class EspClass {
 public:
  void restart();
  uint32_t getFreeHeap();
  uint32_t getMinFreeHeap();
  uint64_t getEfuseMac();
  const char* getChipModel();
};
extern EspClass ESP;

// --- random -----------------------------------------------------------------
long random(long max_exclusive);
long random(long min_inclusive, long max_exclusive);
void randomSeed(unsigned long seed);

// --- math helpers (templates, not macros, so <algorithm> keeps working) -----
template <class T, class U>
inline constexpr typename std::common_type<T, U>::type min(T a, U b) {
  return (a < b) ? static_cast<typename std::common_type<T, U>::type>(a)
                 : static_cast<typename std::common_type<T, U>::type>(b);
}

template <class T, class U>
inline constexpr typename std::common_type<T, U>::type max(T a, U b) {
  return (a > b) ? static_cast<typename std::common_type<T, U>::type>(a)
                 : static_cast<typename std::common_type<T, U>::type>(b);
}

template <class T, class U, class V>
inline constexpr typename std::common_type<T, U, V>::type constrain(T value, U low, V high) {
  using R = typename std::common_type<T, U, V>::type;
  return (R)value < (R)low ? (R)low : ((R)value > (R)high ? (R)high : (R)value);
}

inline long map(long value, long in_min, long in_max, long out_min, long out_max) {
  if (in_max == in_min) return out_min;
  return (value - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

inline float radians(float deg) { return deg * (float)M_PI / 180.0f; }
inline float degrees(float rad) { return rad * 180.0f / (float)M_PI; }
inline double radians(double deg) { return deg * M_PI / 180.0; }
inline double degrees(double rad) { return rad * 180.0 / M_PI; }

// abs() for int/long/float/double comes from <cmath> via <math.h> in C++ mode;
// redeclaring it here would conflict with the library's constexpr overloads.
