#pragma once

#include <stdint.h>
#include <stdio.h>

#include "WString.h"

class IPAddress {
 public:
  IPAddress() : addr_(0) {}
  explicit IPAddress(uint32_t network_order) : addr_(network_order) {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    addr_ = ((uint32_t)a) | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
  }

  uint32_t raw() const { return addr_; }
  operator uint32_t() const { return addr_; }
  uint8_t operator[](int index) const { return (uint8_t)((addr_ >> (8 * index)) & 0xFF); }

  String toString() const {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (*this)[0], (*this)[1], (*this)[2], (*this)[3]);
    return String(buf);
  }

 private:
  uint32_t addr_;  // little-endian byte order, like lwIP's ip4_addr_t
};
