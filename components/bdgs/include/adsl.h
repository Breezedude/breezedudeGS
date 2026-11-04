#pragma once

#include <Arduino.h>
#include "types.h"

// ADS-L 4 SRD-860 (M-band) reception.
//
// Implemented from the EASA specification "Technical Specification for ADS-L
// transmissions using SRD-860 frequency band", ED Decision 2022/024/R, and
// cross-checked against SoftRF's ads-l.h reference implementation.
//
// The physical layer is identical to FLARM legacy - 2-GFSK, 100 kbps chip rate,
// +-50 kHz deviation, BT 0.5, Manchester encoded, on 868.2 and 868.4 MHz. Only
// the sync word and the frame length differ, so the radio configuration is
// shared with the FLARM path.
//
// Unlike FLARM, ADS-L is not encrypted: the payload is only scrambled with XXTEA
// using an all-zero key. It therefore needs neither the station position nor an
// accurate clock to decode.

// On-air bytes after the sync word: 24 frame bytes, Manchester encoded.
#define ADSL_RAW_LEN   48
// Version byte + 20 byte scrambled block + 3 byte CRC.
#define ADSL_FRAME_LEN 24

// Sync word as configured in the radio: Manchester(0x72 0x4B) followed by
// Manchester(0x18), the fixed length byte. Treating the length byte as part of
// the sync word keeps the received payload at a constant 48 bytes.
#define ADSL_SYNCWORD_LEN 6
extern const uint8_t ADSL_SYNCWORD[ADSL_SYNCWORD_LEN];

// Broadcast iConspicuity position report; bit 7 marks unicast payloads.
#define ADSL_TYPE_ICONSPICUITY 0x02

struct AdslTarget {
  uint32_t addr;          // 24 bit sender address
  uint8_t  addrTable;     // address mapping table: 5=ICAO, 6=FLARM, 7=OGN, 8=FANET, 0=random
                          // (adsl_origin_prefix maps these to ICA/FLR/OGN/FNT, RND otherwise)
  uint8_t  aircraftCat;   // raw ADS-L aircraft category (0..31)
  uint8_t  flightState;   // 0=undefined, 1=on ground, 2=airborne
  uint8_t  emergency;     // 0=undefined, 1=no emergency, >1 see spec
  float    lat;
  float    lon;
  float    altitude;      // m above the WGS-84 ellipsoid
  float    speed;         // km/h
  float    climb;         // m/s
  float    course;        // degrees
  bool     hasClimb;      // vertical rate carries the "absent" special value
  bool     relayed;       // packet was forwarded on behalf of the sender
};

// CRC-24 (Mode-S / 1090 MHz ADS-B polynomial) run over len bytes.
uint32_t adsl_crc(const uint8_t* frame, size_t len);

// The value a transmitter must place in the CRC field: computed over the 21 data
// bytes with the three CRC positions treated as zero. Useful for diagnostics -
// adsl_check_crc() alone cannot say what the frame *should* have carried.
uint32_t adsl_crc_expected(const uint8_t* frame);

bool     adsl_check_crc(const uint8_t* frame);

// Manchester decodes ADSL_RAW_LEN on-air bytes into ADSL_FRAME_LEN frame bytes.
void adsl_manchester_decode(const uint8_t* raw, uint8_t* frame);

// Descrambles and parses a CRC-checked frame. Returns false for anything that is
// not a decodable broadcast position report.
bool adsl_decode_frame(const uint8_t* frame, AdslTarget* out);

// Describes why the last adsl_decode_frame() call gave up.
extern char adsl_debug_note[160];

// ADS-L aircraft category mapped onto the FANET types used by the UI.
trck_acft_type adsl_to_fanet_acft(uint8_t cat);

// OGN/APRS call sign prefix for an ADS-L address mapping table value.
const char* adsl_origin_prefix(uint8_t addrTable);
