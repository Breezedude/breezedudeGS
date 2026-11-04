// Arduino-compatible String, backed by std::string.
//
// Only the subset the ported protocol modules actually use is implemented -
// the goal is to compile aprs.cpp, ota_gs.cpp, types.cpp and friends without
// touching them, not to reimplement an Arduino core.
#pragma once

#include <stdint.h>
#include <stdlib.h>

#include <string>

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

class String {
 public:
  String() = default;
  String(const char* s) : s_(s ? s : "") {}
  String(const std::string& s) : s_(s) {}
  String(const String&) = default;
  String(String&&) = default;
  explicit String(char c) : s_(1, c) {}

  String(int value, unsigned char base = DEC) { fromInt((long)value, base); }
  String(unsigned int value, unsigned char base = DEC) { fromUInt((unsigned long)value, base); }
  String(long value, unsigned char base = DEC) { fromInt(value, base); }
  String(unsigned long value, unsigned char base = DEC) { fromUInt(value, base); }
  String(long long value, unsigned char base = DEC) { fromInt((long)value, base); }
  String(unsigned long long value, unsigned char base = DEC) { fromUInt((unsigned long)value, base); }
  String(float value, unsigned char decimals = 2) { fromDouble(value, decimals); }
  String(double value, unsigned char decimals = 2) { fromDouble(value, decimals); }

  String& operator=(const String&) = default;
  String& operator=(String&&) = default;
  String& operator=(const char* s) {
    s_ = s ? s : "";
    return *this;
  }

  // --- capacity / access ---------------------------------------------------
  const char* c_str() const { return s_.c_str(); }
  size_t length() const { return s_.length(); }
  bool isEmpty() const { return s_.empty(); }
  void reserve(size_t n) { s_.reserve(n); }
  void clear() { s_.clear(); }
  char charAt(size_t i) const { return i < s_.size() ? s_[i] : 0; }
  char operator[](size_t i) const { return charAt(i); }
  char& operator[](size_t i) { return s_[i]; }
  const std::string& str() const { return s_; }
  std::string& str() { return s_; }

  // --- concatenation -------------------------------------------------------
  String& operator+=(const String& rhs) {
    s_ += rhs.s_;
    return *this;
  }
  String& operator+=(const char* rhs) {
    if (rhs) s_ += rhs;
    return *this;
  }
  String& operator+=(char rhs) {
    s_ += rhs;
    return *this;
  }
  String& operator+=(int rhs) { return append(String(rhs)); }
  String& operator+=(unsigned int rhs) { return append(String(rhs)); }
  String& operator+=(long rhs) { return append(String(rhs)); }
  String& operator+=(unsigned long rhs) { return append(String(rhs)); }
  String& operator+=(float rhs) { return append(String(rhs)); }
  String& operator+=(double rhs) { return append(String(rhs)); }
  bool concat(const String& rhs) {
    s_ += rhs.s_;
    return true;
  }
  bool concat(const char* rhs) {
    if (rhs) s_ += rhs;
    return true;
  }
  bool concat(char rhs) {
    s_ += rhs;
    return true;
  }

  // --- search / compare ----------------------------------------------------
  int indexOf(char c, size_t from = 0) const { return pos2int(s_.find(c, from)); }
  int indexOf(const String& needle, size_t from = 0) const { return pos2int(s_.find(needle.s_, from)); }
  int indexOf(const char* needle, size_t from = 0) const {
    return pos2int(s_.find(needle ? needle : "", from));
  }
  int lastIndexOf(char c) const { return pos2int(s_.rfind(c)); }
  int lastIndexOf(const String& needle) const { return pos2int(s_.rfind(needle.s_)); }
  bool startsWith(const String& prefix) const { return s_.rfind(prefix.s_, 0) == 0; }
  bool startsWith(const char* prefix) const { return prefix && s_.rfind(prefix, 0) == 0; }
  bool endsWith(const String& suffix) const {
    return s_.size() >= suffix.s_.size() &&
           s_.compare(s_.size() - suffix.s_.size(), suffix.s_.size(), suffix.s_) == 0;
  }
  bool equals(const String& rhs) const { return s_ == rhs.s_; }
  bool equals(const char* rhs) const { return rhs && s_ == rhs; }
  bool equalsIgnoreCase(const String& rhs) const;

  // --- modification --------------------------------------------------------
  String substring(size_t from) const { return from >= s_.size() ? String() : String(s_.substr(from)); }
  String substring(size_t from, size_t to) const;
  void trim();
  void toUpperCase();
  void toLowerCase();
  void remove(size_t index) {
    if (index < s_.size()) s_.erase(index);
  }
  void remove(size_t index, size_t count) {
    if (index < s_.size()) s_.erase(index, count);
  }
  void replace(char from, char to);
  void replace(const String& from, const String& to);

  // --- conversion ----------------------------------------------------------
  long toInt() const { return strtol(s_.c_str(), nullptr, 10); }
  float toFloat() const { return strtof(s_.c_str(), nullptr); }
  double toDouble() const { return strtod(s_.c_str(), nullptr); }

 private:
  static int pos2int(size_t pos) { return pos == std::string::npos ? -1 : (int)pos; }
  String& append(const String& rhs) {
    s_ += rhs.s_;
    return *this;
  }
  void fromInt(long value, unsigned char base);
  void fromUInt(unsigned long value, unsigned char base);
  void fromDouble(double value, unsigned char decimals);

  std::string s_;
};

inline String operator+(const String& a, const String& b) {
  String r(a);
  r += b;
  return r;
}
inline String operator+(const String& a, const char* b) {
  String r(a);
  r += b;
  return r;
}
inline String operator+(const char* a, const String& b) {
  String r(a);
  r += b;
  return r;
}
inline String operator+(const String& a, char b) {
  String r(a);
  r += b;
  return r;
}
inline String operator+(const String& a, int b) {
  String r(a);
  r += b;
  return r;
}
inline String operator+(const String& a, unsigned int b) {
  String r(a);
  r += b;
  return r;
}
inline String operator+(const String& a, long b) {
  String r(a);
  r += b;
  return r;
}
inline String operator+(const String& a, unsigned long b) {
  String r(a);
  r += b;
  return r;
}
inline String operator+(const String& a, float b) {
  String r(a);
  r += b;
  return r;
}
inline String operator+(const String& a, double b) {
  String r(a);
  r += b;
  return r;
}

inline bool operator==(const String& a, const String& b) { return a.str() == b.str(); }
inline bool operator==(const String& a, const char* b) { return b && a.str() == b; }
inline bool operator==(const char* a, const String& b) { return a && b.str() == a; }
inline bool operator!=(const String& a, const String& b) { return !(a == b); }
inline bool operator!=(const String& a, const char* b) { return !(a == b); }
inline bool operator<(const String& a, const String& b) { return a.str() < b.str(); }
inline bool operator>(const String& a, const String& b) { return a.str() > b.str(); }
