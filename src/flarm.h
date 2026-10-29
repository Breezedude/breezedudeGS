#pragma once

#include <Arduino.h>
#include "types.h"

// FLARM "legacy" receive support.
//
// Protocol parameters, keys and the decoding chain are taken from GXAirCom
// (lib/FLARM/FlarmRadio.cpp, lib/FANETLORA/radio/LoRa.cpp), which in turn
// documents them in lib/FLARM/v6.txt and v7.txt. Two packet generations are on
// the air today: v7 (type == 2, current devices) and v6 (type == 0, older ones).

// Bytes as they arrive from the radio: 26 payload bytes, Manchester encoded.
#define FLARM_RAW_LEN     52
// Decoded payload: 24 data bytes + 2 bytes CRC.
#define FLARM_PAYLOAD_LEN 26

// Zone 1 (Europe) hops between these two channels within one second.
#define FLARM_FREQ_LOW_MHZ  868.2f
#define FLARM_FREQ_HIGH_MHZ 868.4f

struct FlarmTarget {
  uint32_t addr;         // 24 bit device address
  uint8_t  addrType;     // 0 = random, 1 = ICAO, 2 = FLARM, 3 = OGN
  uint8_t  aircraftType; // raw FLARM aircraft type (0..15), as OGN expects it
  float    lat;
  float    lon;
  float    altitude;     // m, WGS84 ellipsoid minus geoid separation
  float    speed;        // km/h
  float    climb;        // m/s
  float    course;       // degrees
  bool     stealth;
  bool     noTrack;
  bool     onGround;
  uint8_t  protoVersion; // 6 or 7
};

// Manchester-decodes FLARM_RAW_LEN on-air bytes into FLARM_PAYLOAD_LEN bytes.
void flarm_manchester_decode(const uint8_t* raw, uint8_t* out);

// CRC-CCITT over the 24 data bytes, seeded with the fixed 0x31 0xFA 0xB6 prefix.
uint16_t flarm_crc(const uint8_t* payload);

// Verifies that CRC against the trailing two bytes (big endian).
bool flarm_check_crc(const uint8_t* payload);

// Decrypts and unpacks a CRC-checked payload. refLat/refLon must be the station
// position - the v7 format transmits only the low bits of latitude/longitude, so
// the receiver position is required to reconstruct them. The transmit second is
// part of the key schedule and only known approximately, so the neighbouring
// seconds are tried; candidates further than 100 km away are rejected as
// mis-decodes, mirroring GXAirCom's plausibility check.
bool flarm_decode_payload(const uint8_t* payload, float refLat, float refLon, time_t nowUtc, FlarmTarget* out);

// Describes why the last flarm_decode_payload() call gave up, using the attempt
// with the unmodified timestamp. Only meaningful directly after a failed call.
extern char flarm_debug_note[160];

// FLARM aircraft type (0..15) mapped onto the FANET types used by the UI.
trck_acft_type flarm_to_fanet_acft(uint8_t flarmType);

// OGN/APRS call sign prefix for a FLARM address type.
const char* flarm_origin_prefix(uint8_t addrType);
