// Minimal Arduino Print/Stream pair. Used by the serial mux, the TCP client
// and the HTTP response reader.
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "WString.h"

class Print {
 public:
  virtual ~Print() = default;

  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size);
  size_t write(const char* s) { return s ? write((const uint8_t*)s, strlen(s)) : 0; }
  size_t write(const char* buffer, size_t size) { return write((const uint8_t*)buffer, size); }
  virtual void flush() {}

  size_t print(const String& s) { return write((const uint8_t*)s.c_str(), s.length()); }
  size_t print(const char* s) { return write(s); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(unsigned char value, int base = DEC) { return print(String((unsigned long)value, base)); }
  size_t print(int value, int base = DEC) { return print(String((long)value, base)); }
  size_t print(unsigned int value, int base = DEC) { return print(String((unsigned long)value, base)); }
  size_t print(long value, int base = DEC) { return print(String(value, base)); }
  size_t print(unsigned long value, int base = DEC) { return print(String(value, base)); }
  size_t print(double value, int digits = 2) { return print(String(value, (unsigned char)digits)); }

  size_t println() { return write("\r\n"); }
  size_t println(const String& s) { return print(s) + println(); }
  size_t println(const char* s) { return print(s) + println(); }
  size_t println(char c) { return print(c) + println(); }
  size_t println(unsigned char value, int base = DEC) { return print(value, base) + println(); }
  size_t println(int value, int base = DEC) { return print(value, base) + println(); }
  size_t println(unsigned int value, int base = DEC) { return print(value, base) + println(); }
  size_t println(long value, int base = DEC) { return print(value, base) + println(); }
  size_t println(unsigned long value, int base = DEC) { return print(value, base) + println(); }
  size_t println(double value, int digits = 2) { return print(value, digits) + println(); }

  size_t printf(const char* format, ...) __attribute__((format(printf, 2, 3)));
  size_t vprintf(const char* format, va_list args);
};

class Stream : public Print {
 public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
  virtual int read(uint8_t* buffer, size_t size);
  size_t readBytes(uint8_t* buffer, size_t size);
  void setTimeout(unsigned long timeout_ms) { timeout_ms_ = timeout_ms; }
  unsigned long getTimeout() const { return timeout_ms_; }

 protected:
  unsigned long timeout_ms_ = 1000;
};
