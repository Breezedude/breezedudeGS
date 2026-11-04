#include "WString.h"

#include <ctype.h>
#include <stdio.h>

void String::fromInt(long value, unsigned char base) {
  char buf[36];
  if (base == HEX) {
    snprintf(buf, sizeof(buf), "%lx", (unsigned long)value);
  } else if (base == OCT) {
    snprintf(buf, sizeof(buf), "%lo", (unsigned long)value);
  } else if (base == BIN) {
    fromUInt((unsigned long)value, base);
    return;
  } else {
    snprintf(buf, sizeof(buf), "%ld", value);
  }
  s_ = buf;
}

void String::fromUInt(unsigned long value, unsigned char base) {
  if (base == BIN) {
    char buf[33];
    int i = 32;
    buf[i] = 0;
    do {
      buf[--i] = (char)('0' + (value & 1u));
      value >>= 1;
    } while (value && i > 0);
    s_ = &buf[i];
    return;
  }

  char buf[36];
  if (base == HEX) {
    snprintf(buf, sizeof(buf), "%lx", value);
  } else if (base == OCT) {
    snprintf(buf, sizeof(buf), "%lo", value);
  } else {
    snprintf(buf, sizeof(buf), "%lu", value);
  }
  s_ = buf;
}

void String::fromDouble(double value, unsigned char decimals) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%.*f", (int)decimals, value);
  s_ = buf;
}

bool String::equalsIgnoreCase(const String& rhs) const {
  if (s_.size() != rhs.s_.size()) return false;
  for (size_t i = 0; i < s_.size(); i++) {
    if (tolower((unsigned char)s_[i]) != tolower((unsigned char)rhs.s_[i])) return false;
  }
  return true;
}

String String::substring(size_t from, size_t to) const {
  if (from >= s_.size() || to <= from) return String();
  if (to > s_.size()) to = s_.size();
  return String(s_.substr(from, to - from));
}

void String::trim() {
  size_t begin = 0;
  while (begin < s_.size() && isspace((unsigned char)s_[begin])) begin++;
  size_t end = s_.size();
  while (end > begin && isspace((unsigned char)s_[end - 1])) end--;
  s_ = s_.substr(begin, end - begin);
}

void String::toUpperCase() {
  for (char& c : s_) c = (char)toupper((unsigned char)c);
}

void String::toLowerCase() {
  for (char& c : s_) c = (char)tolower((unsigned char)c);
}

void String::replace(char from, char to) {
  for (char& c : s_) {
    if (c == from) c = to;
  }
}

void String::replace(const String& from, const String& to) {
  if (from.s_.empty()) return;
  size_t pos = 0;
  while ((pos = s_.find(from.s_, pos)) != std::string::npos) {
    s_.replace(pos, from.s_.size(), to.s_);
    pos += to.s_.size();
  }
}
