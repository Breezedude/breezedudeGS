#include "adsl.h"
#include "manchester.h"

#include <math.h>
#include <stdarg.h>

// Manchester(0x72) = 0x95 0xA6, Manchester(0x4B) = 0x9A 0x65,
// Manchester(0x18) = 0xA9 0x6A  (spec C.2.4 sync word plus the fixed length byte)
const uint8_t ADSL_SYNCWORD[ADSL_SYNCWORD_LEN] = { 0x95, 0xA6, 0x9A, 0x65, 0xA9, 0x6A };

char adsl_debug_note[160] = {0};

static void note(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(adsl_debug_note, sizeof(adsl_debug_note), fmt, ap);
  va_end(ap);
}

// ---------------------------------------------------------------------------
// Frame layout
// ---------------------------------------------------------------------------

// The scrambled block is processed as 32-bit words, so it has to be aligned.
// The three leading bytes stand in for the two sync bytes and the length byte
// that the radio already consumed, and exist only to push Payload to offset 4.
//
// Both attributes are needed: packed keeps the members contiguous, aligned(4)
// makes the struct itself 4-byte aligned. Without the latter, packed would allow
// the whole struct at an odd address and the uint32_t access below would be
// unaligned - which on Xtensa and RISC-V reads garbage or faults.
typedef struct {
  uint8_t pad[3];
  uint8_t Version;      // Version[4]/Signature[1]/Key[2]/Reserved[1]
  uint8_t Payload[20];  // scrambled: Type[1]/Address[4]/Meta[2]/Position[11]/Integrity[2]
  uint8_t CRC24[3];
} __attribute__((packed, aligned(4))) adsl_frame_t;

static_assert(offsetof(adsl_frame_t, Version) == 3, "ADS-L frame layout shifted");
static_assert(offsetof(adsl_frame_t, Payload) % 4 == 0, "ADS-L payload not 32-bit aligned");

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

// Variable-resolution decoding used for speed, altitude and vertical rate: the
// top bits select one of four ranges with increasing step size.
static uint32_t uns_vr_decode(uint32_t value, uint8_t bits) {
  const uint32_t thres = 1u << bits;
  uint8_t range = value >> bits;
  value &= thres - 1;
  if (range == 0) return           value;
  if (range == 1) return   thres + 1 + (value << 1);
  if (range == 2) return 3*thres + 2 + (value << 2);
  return                  7*thres + 4 + (value << 3);
}

static int32_t sign_vr_decode(uint32_t value, uint8_t bits) {
  const uint32_t signMask = 1u << (bits + 2);
  bool negative = value & signMask;
  int32_t v = (int32_t)uns_vr_decode(value & (signMask - 1), bits);
  return negative ? -v : v;
}

// XXTEA with an all-zero key, which reduces the round function to this form.
static uint32_t xxtea_mx_key0(uint32_t y, uint32_t z, uint32_t sum) {
  return ((((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + z));
}

static void xxtea_decrypt_key0(uint32_t* data, uint8_t words, uint8_t loops) {
  const uint32_t delta = 0x9e3779b9;
  uint32_t sum = (uint32_t)loops * delta;
  uint32_t y = data[0], z;
  for (; loops; loops--) {
    for (uint8_t p = words - 1; p; p--) {
      z = data[p - 1];
      y = data[p] -= xxtea_mx_key0(y, z, sum);
    }
    z = data[words - 1];
    y = data[0] -= xxtea_mx_key0(y, z, sum);
    sum -= delta;
  }
}

static int32_t sign_extend_24(uint32_t v) {
  return (v & 0x800000) ? (int32_t)(v | 0xFF000000u) : (int32_t)v;
}

static uint32_t get3bytes(const uint8_t* b) {
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
}

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

void adsl_manchester_decode(const uint8_t* raw, uint8_t* frame) {
  manchester_decode(raw, frame, ADSL_FRAME_LEN);
}

// Mode-S / 1090 MHz ADS-B polynomial, per spec D.1.2.
static uint32_t crc24_polypass(uint32_t crc, uint8_t input) {
  const uint32_t poly = 0xFFFA0480;
  crc |= input;
  for (uint8_t bit = 0; bit < 8; bit++) {
    if (crc & 0x80000000) crc ^= poly;
    crc <<= 1;
  }
  return crc;
}

uint32_t adsl_crc(const uint8_t* frame, size_t len) {
  uint32_t crc = 0;
  for (size_t i = 0; i < len; i++) crc = crc24_polypass(crc, frame[i]);
  return crc >> 8;
}

uint32_t adsl_crc_expected(const uint8_t* frame) {
  uint32_t crc = 0;
  for (size_t i = 0; i < ADSL_FRAME_LEN - 3; i++) crc = crc24_polypass(crc, frame[i]);
  // The three CRC positions take part in the calculation as zero bytes.
  crc = crc24_polypass(crc, 0);
  crc = crc24_polypass(crc, 0);
  crc = crc24_polypass(crc, 0);
  return crc >> 8;
}

bool adsl_check_crc(const uint8_t* frame) {
  // Running the CRC across the data *and* the three transmitted CRC bytes has to
  // come out zero; that is cheaper than recomputing and comparing.
  return adsl_crc(frame, ADSL_FRAME_LEN) == 0;
}

trck_acft_type adsl_to_fanet_acft(uint8_t cat) {
  // Categories per spec G.1.3.
  switch (cat) {
    case 1:  return acft_Powered_Aircraft; // light fixed wing
    case 2:  return acft_Powered_Aircraft; // small to heavy fixed wing
    case 3:  return acft_Helicopter;       // rotorcraft
    case 4:  return acft_Glider;
    case 5:  return acft_Balloon;          // lighter-than-air
    case 6:  return acft_Powered_Aircraft; // ultralight
    case 7:  return acft_Paraglider;       // hang-glider / paraglider
    case 8:  return acft_Other;            // parachutist / skydiver
    case 9:  return acft_UAV;              // eVTOL / UAM
    case 10: return acft_Helicopter;       // gyrocopter
    case 11:
    case 12:
    case 13: return acft_UAV;              // UAS categories
    default: return acft_Other;
  }
}

const char* adsl_origin_prefix(uint8_t addrTable) {
  // Address mapping table per spec F.2.2.
  switch (addrTable) {
    case 5:  return "ICA";
    case 6:  return "FLR";
    case 7:  return "OGN";
    case 8:  return "FNT";
    default: return "RND"; // random/privacy and manufacturer pages
  }
}

// ---------------------------------------------------------------------------
// Frame decoding
// ---------------------------------------------------------------------------

bool adsl_decode_frame(const uint8_t* frame, AdslTarget* out) {
  adsl_debug_note[0] = '\0';

  adsl_frame_t f;
  memcpy(&f.Version, frame, ADSL_FRAME_LEN);

  uint8_t protoVersion = f.Version & 0x0F;
  if (protoVersion != 0) {
    note("protocol version %u (want 0)", protoVersion);
    return false;
  }
  // Spec E.1.3: only key 0 (all zeros) is defined; a packet scrambled with
  // another key would descramble to noise that could still pass the checks
  // below by chance.
  uint8_t keyIndex = (f.Version >> 5) & 0x03;
  if (keyIndex != 0) {
    note("key index %u - cannot descramble", keyIndex);
    return false;
  }

  xxtea_decrypt_key0((uint32_t*)f.Payload, 5, 6);

  const uint8_t* p = f.Payload;
  uint8_t type = p[0];
  if (type & 0x80) {
    note("unicast payload type 0x%02X - not a broadcast report", type);
    return false;
  }
  if (type != ADSL_TYPE_ICONSPICUITY) {
    note("payload type %u (only 2 = iConspicuity is decoded)", type);
    return false;
  }

  uint32_t addrWord = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                      ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
  out->addr      = (addrWord >> 6) & 0x00FFFFFF;
  out->addrTable = p[1] & 0x3F;
  out->relayed   = (addrWord >> 31) & 1;

  out->flightState = (p[5] >> 6) & 0x03;
  out->aircraftCat = p[6] & 0x1F;
  out->emergency   = (p[6] >> 5) & 0x07;

  const uint8_t* pos = &p[7];

  // Spec G.1.5: 0xFFFFFF means no position fix. Sign-extended it would read as
  // -1 LSB, a point next to 0/0 that passes the plausibility check below.
  const uint32_t latRaw = get3bytes(pos);
  const uint32_t lonRaw = get3bytes(pos + 3);
  if (latRaw == 0xFFFFFF || lonRaw == 0xFFFFFF) {
    note("no position fix");
    return false;
  }

  // Latitude 1 deg / 93206, longitude 1 deg / 46603 - the same scaling FANET
  // uses, see spec G.1 field table.
  out->lat = (float)sign_extend_24(latRaw) / 93206.0f;
  out->lon = (float)sign_extend_24(lonRaw) / 46603.0f;

  out->speed = (float)uns_vr_decode(pos[6], 6) * 0.25f * 3.6f; // 0.25 m/s -> km/h

  uint32_t altWord = ((uint32_t)(pos[8] & 0x3F) << 8) | pos[7];
  out->altitude = (float)((int32_t)uns_vr_decode(altWord, 12) - 320);

  uint32_t climbWord = ((uint32_t)(pos[9] & 0x7F) << 2) | (pos[8] >> 6);
  out->hasClimb = (climbWord != 0x100); // reserved value meaning "absent"
  out->climb    = out->hasClimb ? (float)sign_vr_decode(climbWord, 6) * 0.125f : 0.0f;

  uint16_t trackWord = ((uint16_t)pos[10] << 1) | (pos[9] >> 7);
  out->course = (float)trackWord * (360.0f / 512.0f);

  if (out->lat < -90.0f || out->lat > 90.0f || out->lon < -180.0f || out->lon > 180.0f) {
    note("implausible position %.5f/%.5f", out->lat, out->lon);
    return false;
  }

  return true;
}
