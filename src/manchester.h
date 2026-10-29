#pragma once

#include <Arduino.h>

// Both FLARM and ADS-L Manchester-encode sync word, payload and CRC on the air,
// which doubles the byte count. Neither the SX126x nor the SX127x can undo that
// in hardware, so the radio is configured for the doubled length and the payload
// is decoded here.
//
// Lower nibble of each table entry holds the data bits, the upper nibble the
// error pattern (a symbol that was neither 01 nor 10). The error nibble is
// deliberately ignored: both protocols carry a CRC that catches those cases,
// and rejecting early would only lose otherwise recoverable frames.
extern const uint8_t ManchesterDecodeTable[0x100];

// Decodes 2*len on-air bytes into len payload bytes.
inline void manchester_decode(const uint8_t* raw, uint8_t* out, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uint8_t hi = ManchesterDecodeTable[raw[2 * i]];
    uint8_t lo = ManchesterDecodeTable[raw[2 * i + 1]];
    out[i] = ((hi & 0x0F) << 4) | (lo & 0x0F);
  }
}
