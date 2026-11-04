// Streams JSON through a small fixed buffer, so what it costs in RAM does not
// depend on how much data there is. The sink decides where the bytes go; a sink
// that fails makes every further write a no-op.
//
// Also an ArduinoJson writer: serializeJson(doc, out).
#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

class BdJsonOut {
 public:
  using Sink = bool (*)(void* ctx, const char* data, size_t len);

  BdJsonOut(Sink sink, void* ctx) : sink_(sink), ctx_(ctx) {}

  size_t write(uint8_t c) { return write(&c, 1); }

  size_t write(const uint8_t* data, size_t len) {
    size_t left = len;
    while (ok_ && left) {
      size_t n = sizeof(buf_) - used_;
      if (n > left) n = left;
      memcpy(buf_ + used_, data, n);
      used_ += n;
      data += n;
      left -= n;
      if (used_ == sizeof(buf_)) flush();
    }
    return ok_ ? len : 0;
  }

  void text(const char* s) { write((const uint8_t*)s, strlen(s)); }

  void doc(const JsonDocument& d) { serializeJson(d, *this); }

  bool flush() {
    if (ok_ && used_) ok_ = sink_(ctx_, buf_, used_);
    used_ = 0;
    return ok_;
  }

  bool ok() const { return ok_; }

 private:
  Sink sink_;
  void* ctx_;
  char buf_[1024];
  size_t used_ = 0;
  bool ok_ = true;
};
