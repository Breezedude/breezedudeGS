#include "flarm.h"
#include "helper.h"

#include <math.h>
#include <stdarg.h>
#include "manchester.h"

// ---------------------------------------------------------------------------
// Keys and tables (GXAirCom lib/FLARM/FlarmRadio.h)
// ---------------------------------------------------------------------------

static const uint32_t FLARM_TABLE[8] = {
  0xe43276df, 0xdca83759, 0x9802b8ac, 0x4675a56b,
  0xfc78ea65, 0x804b90ea, 0xb76542cd, 0x329dfa32
};

#define FLARM_KEY2 0x045d9f3b
#define FLARM_KEY3 0x87b562f4
#define FLARM_KEY4 0x956F6C77

static const uint32_t FLARM_KEY5[4] = { 0xA5F9B21C, 0xAB3F9D12, 0xC6F34E34, 0xD72FA378 };

#define XXTEA_DELTA  0x9e3779b9
#define XXTEA_ROUNDS 6

// Turn rate encoding of the v6 packet.
#define TURN_RATE_0          0
#define TURN_RATE_ON_GROUND  1
#define TURN_RATE_RIGHT_TURN 4
#define TURN_RATE_NO_TURN    5
#define TURN_RATE_LEFT_TURN  7

// Longitude divisor per degree of latitude, used by the v7 format to keep the
// transmitted resolution roughly constant towards the poles.
static const uint16_t lon_div_table[] = {
   53,  53,  54,  54,  55,  55,
   56,  56,  57,  57,  58,  58,  59,  59,  60,  60,
   61,  61,  62,  62,  63,  63,  64,  64,  65,  65,
   67,  68,  70,  71,  73,  74,  76,  77,  79,  80,
   82,  83,  85,  86,  88,  89,  91,  94,  98, 101,
  105, 108, 112, 115, 119, 122, 126, 129, 137, 144,
  152, 159, 167, 174, 190, 205, 221, 236, 252,
  267, 299, 330, 362, 425, 489, 552, 616, 679, 743, 806, 806
};

// ---------------------------------------------------------------------------
// Packet layouts. The packed attribute is essential: it makes GCC allocate the
// bit fields as one contiguous bit stream instead of padding to storage units.
// ---------------------------------------------------------------------------

typedef struct {
  unsigned int addr:24;
  unsigned int type:4;      // 0 = old protocol, 2 = new protocol, 1 = other messages
  unsigned int addr_type:3;
  unsigned int zero1:1;

  int          vs:10;
  unsigned int turnrate:3;
  unsigned int stealth:1;
  unsigned int no_track:1;
  unsigned int parity:1;
  unsigned int gps:12;
  unsigned int aircraft_type:4;

  unsigned int lat:19;
  unsigned int alt:13;

  unsigned int lon:20;
  unsigned int zero2:10;
  unsigned int smult:2;

  int8_t ns[4];
  int8_t ew[4];
} __attribute__((packed)) flarm_v6_packet_t;

typedef struct {
  unsigned int addr:24;
  unsigned int type:4;
  unsigned int addr_type:3;
  unsigned int _unk1:1;

  unsigned int _unk2:22;    // always 0
  unsigned int stealth:1;
  unsigned int no_track:1;
  unsigned int _unk3:2;     // always 3
  unsigned int _unk4:2;     // always 0
  unsigned int _unk5:2;     // always 3
  unsigned int _unk6:2;     // always 0

  unsigned int _unk7:2;
  unsigned int tstamp:4;    // low 4 bits of the unix timestamp
  unsigned int aircraft_type:4;
  unsigned int _unk8:1;
  unsigned int alt:13;      // meters + 1000, enscaled(12,1)

  unsigned int lat:20;      // rounded, MS bits removed
  unsigned int lon:20;      // rounded, MS bits removed
  int          turn:9;
  unsigned int hs:10;       // m/s times 10, enscaled(8,2)
  int          vs:9;        // m/s times 10, enscaled(6,2)
  unsigned int course:10;   // degrees times 2
  unsigned int airborne:2;  // 1 = stationary, 2 = moving, 3 = circling

  unsigned int hp:6;
  unsigned int vp:5;
  unsigned int _unk9:5;
  unsigned int _unk10:8;
} __attribute__((packed)) flarm_v7_packet_t;

// The decoders overlay these structs on the raw payload and XXTEA operates on
// them word-wise, so a padded layout would silently produce garbage instead of
// failing loudly. Both must be exactly the 24 data bytes of a frame.
static_assert(sizeof(flarm_v6_packet_t) == 24, "v6 packet layout padded");
static_assert(sizeof(flarm_v7_packet_t) == 24, "v7 packet layout padded");

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

static uint16_t crc_ccitt_update(uint16_t crc, uint8_t c) {
  crc ^= (uint16_t)c << 8;
  for (uint8_t i = 0; i < 8; i++) {
    crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static int descale(unsigned int value, unsigned int mbits, unsigned int ebits) {
  unsigned int offset   = (1 << mbits);
  unsigned int signbit  = (offset << ebits);
  unsigned int negative = (value & signbit);

  value &= (signbit - 1);

  if (value >= offset) {
    unsigned int exp = value >> mbits;
    value &= (offset - 1);
    value += offset;
    value <<= exp;
    value -= offset;
  }

  return (negative ? -(int)value : value);
}

static uint32_t obscure(uint32_t key, uint32_t seed) {
  uint32_t m1 = seed * (key ^ (key >> 16));
  uint32_t m2 = (seed * (m1 ^ (m1 >> 16)));
  return m2 ^ (m2 >> 16);
}

static void make_v6_key(uint32_t key[4], uint32_t timestamp, uint32_t address) {
  for (int8_t i = 0; i < 4; i++) {
    int8_t ndx = ((timestamp >> 23) & 1) ? i + 4 : i;
    key[i] = obscure(FLARM_TABLE[ndx] ^ ((timestamp >> 6) ^ address), FLARM_KEY2) ^ FLARM_KEY3;
  }
}

static void btea(uint32_t *v, int8_t n, const uint32_t key[4]) {
  uint32_t y, z, sum;
  uint32_t p, rounds, e;

  #define MX (((z >> 5 ^ y << 2) + (y >> 3 ^ z << 4)) ^ ((sum ^ y) + (key[(p & 3) ^ e] ^ z)))

  if (n > 1) {
    rounds = XXTEA_ROUNDS;
    sum = 0;
    z = v[n - 1];
    do {
      sum += XXTEA_DELTA;
      e = (sum >> 2) & 3;
      for (p = 0; p < (uint32_t)n - 1; p++) {
        y = v[p + 1];
        z = v[p] += MX;
      }
      y = v[0];
      z = v[n - 1] += MX;
    } while (--rounds);
  } else if (n < -1) {
    n = -n;
    rounds = XXTEA_ROUNDS;
    sum = rounds * XXTEA_DELTA;
    y = v[0];
    do {
      e = (sum >> 2) & 3;
      for (p = n - 1; p > 0; p--) {
        z = v[p - 1];
        y = v[p] -= MX;
      }
      z = v[n - 1];
      y = v[0] -= MX;
      sum -= XXTEA_DELTA;
    } while (--rounds);
  }

  #undef MX
}

static void make_v7_key(uint32_t key[4]) {
  uint8_t *bkeys = (uint8_t *)&key[0];
  int p, q, x, y, z, sum;

  x = bkeys[15];
  sum = 0;
  q = 2;

  do {
    sum += XXTEA_DELTA;
    for (p = 0; p < 16; p++) {
      z = x & 0xFF;
      y = bkeys[(p + 1) % 16];
      x = bkeys[p];
      x += ((((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ (sum ^ y));
      bkeys[p] = (uint8_t)x;
    }
  } while (--q > 0);
}

static uint8_t byte_parity(uint32_t x) {
  uint8_t parity = 0;
  while (x > 0) {
    if (x & 0x1) parity++;
    x >>= 1;
  }
  return (parity % 2);
}

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

void flarm_manchester_decode(const uint8_t* raw, uint8_t* out) {
  manchester_decode(raw, out, FLARM_PAYLOAD_LEN);
}

uint16_t flarm_crc(const uint8_t* payload) {
  uint16_t crc = 0xffff;
  // The checksum is seeded with a fixed three byte prefix before the payload.
  crc = crc_ccitt_update(crc, 0x31);
  crc = crc_ccitt_update(crc, 0xFA);
  crc = crc_ccitt_update(crc, 0xB6);
  for (int i = 0; i < 24; i++) {
    crc = crc_ccitt_update(crc, payload[i]);
  }
  return crc;
}

bool flarm_check_crc(const uint8_t* payload) {
  uint16_t received = ((uint16_t)payload[24] << 8) | (uint16_t)payload[25];
  return flarm_crc(payload) == received;
}

trck_acft_type flarm_to_fanet_acft(uint8_t flarmType) {
  switch (flarmType) {
    case 1:  return acft_Glider;            // glider / motor glider
    case 2:  return acft_Powered_Aircraft;  // tow plane
    case 3:  return acft_Helicopter;
    case 5:  return acft_Powered_Aircraft;  // drop plane
    case 6:  return acft_Hangglider;
    case 7:  return acft_Paraglider;
    case 8:  return acft_Powered_Aircraft;  // reciprocating engine
    case 9:  return acft_Powered_Aircraft;  // jet / turboprop
    case 11: return acft_Balloon;
    case 12: return acft_Balloon;           // airship
    case 13: return acft_UAV;
    default: return acft_Other;             // incl. 4 parachute, 15 static object
  }
}

const char* flarm_origin_prefix(uint8_t addrType) {
  switch (addrType) {
    case 1:  return "ICA";
    case 2:  return "FLR";
    case 3:  return "OGN";
    default: return "RND";
  }
}

// ---------------------------------------------------------------------------
// Packet decoders
// ---------------------------------------------------------------------------

char flarm_debug_note[160] = {0};

// Only the attempt with the unshifted timestamp records a reason; the retries
// with neighbouring seconds fail for uninteresting reasons and would just
// overwrite the useful message.
static bool note_enabled = false;

static void note(const char* fmt, ...) {
  if (!note_enabled) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(flarm_debug_note, sizeof(flarm_debug_note), fmt, ap);
  va_end(ap);
}

static bool decode_v6(uint8_t* pktBuf, float refLat, float refLon, uint32_t timestamp, FlarmTarget* out) {
  flarm_v6_packet_t *pkt = (flarm_v6_packet_t *)pktBuf;

  uint32_t key[4];
  make_v6_key(key, timestamp, (pkt->addr << 8) & 0xffffff);
  btea((uint32_t *)pktBuf + 1, -5, key);

  uint8_t pkt_parity = 0;
  for (size_t ndx = 0; ndx < sizeof(flarm_v6_packet_t); ndx++) {
    pkt_parity += byte_parity(pktBuf[ndx]);
  }
  if (pkt_parity % 2) { note("v6: odd parity over the decrypted packet"); return false; }

  if (pkt->addr == 0)          { note("v6: addr is 0"); return false; }
  if (pkt->aircraft_type == 0) { note("v6: aircraft_type is 0"); return false; }
  if (pkt->type != 0)          { note("v6: type=%u (want 0)", pkt->type); return false; }
  if (pkt->zero1 != 0)         { note("v6: zero1=%u (want 0)", pkt->zero1); return false; }
  if (pkt->zero2 != 0)         { note("v6: zero2=%u (want 0)", pkt->zero2); return false; }

  if (pkt->turnrate != TURN_RATE_0 && pkt->turnrate != TURN_RATE_ON_GROUND &&
      pkt->turnrate != TURN_RATE_RIGHT_TURN && pkt->turnrate != TURN_RATE_NO_TURN &&
      pkt->turnrate != TURN_RATE_LEFT_TURN) {
    note("v6: turnrate=%u (want 0/1/4/5/7)", pkt->turnrate);
    return false;
  }

  int32_t round_lat = (int32_t)(refLat * 1e7) >> 7;
  int32_t lat = (pkt->lat - round_lat) % (uint32_t)0x080000;
  if (lat >= 0x040000) lat -= 0x080000;
  lat = ((lat + round_lat) << 7);

  int32_t round_lon = (int32_t)(refLon * 1e7) >> 7;
  int32_t lon = (pkt->lon - round_lon) % (uint32_t)0x100000;
  if (lon >= 0x080000) lon -= 0x100000;
  lon = ((lon + round_lon) << 7);

  int32_t ns = (pkt->ns[0] + pkt->ns[1] + pkt->ns[2] + pkt->ns[3]) / 4;
  int32_t ew = (pkt->ew[0] + pkt->ew[1] + pkt->ew[2] + pkt->ew[3]) / 4;
  float speed4 = sqrtf((float)(ew * ew + ns * ns)) * (1 << pkt->smult);

  float direction = 0;
  if (speed4 > 0) {
    direction = atan2f((float)ns, (float)ew) * 180.0f / PI; // -180 ... 180
    // convert from math angle into course relative to north
    direction = (direction <= 90.0f ? 90.0f - direction : 450.0f - direction);
  }

  uint16_t vs_u16 = pkt->vs;
  int16_t vs_i16 = (int16_t)(vs_u16 | (vs_u16 & (1 << 9) ? 0xFC00U : 0));
  int16_t vs10 = vs_i16 << pkt->smult;

  out->addr         = pkt->addr;
  out->addrType     = pkt->addr_type;
  out->aircraftType = pkt->aircraft_type;
  out->lat          = (float)lat / 1e7f;
  out->lon          = (float)lon / 1e7f;
  out->altitude     = (float)pkt->alt;
  out->speed        = speed4 / (4.0f * 0.277778f); // 4 s vector, m/s -> km/h
  out->course       = direction;
  out->climb        = ((float)vs10) / 10.0f;
  out->stealth      = pkt->stealth;
  out->noTrack      = pkt->no_track;
  out->onGround     = (pkt->turnrate == TURN_RATE_ON_GROUND);
  out->protoVersion = 6;
  return true;
}

static bool decode_v7(uint8_t* pktBuf, float refLat, float refLon, uint32_t timestamp, FlarmTarget* out) {
  uint32_t *wpkt = (uint32_t *)pktBuf;
  flarm_v7_packet_t *pkt = (flarm_v7_packet_t *)pktBuf;
  uint32_t key_v7[4];

  btea(&wpkt[2], -4, FLARM_KEY5);

  key_v7[0] = wpkt[0];
  key_v7[1] = wpkt[1];
  key_v7[2] = timestamp >> 4;
  key_v7[3] = FLARM_KEY4;
  make_v7_key(key_v7);

  wpkt[2] ^= key_v7[0];
  wpkt[3] ^= key_v7[1];
  wpkt[4] ^= key_v7[2];
  wpkt[5] ^= key_v7[3];

  // Fields that are constant in every valid packet - the cheapest way to tell a
  // successful decode from a wrong timestamp guess. A hand-written encoder that
  // leaves these at zero fails here, so report all of them at once.
  if (pkt->_unk1 != 0 || pkt->_unk2 != 0 || pkt->_unk3 != 3 ||
      pkt->_unk4 != 0 || pkt->_unk5 != 3 || pkt->_unk6 != 0) {
    note("v7 constants _unk1..6=%u,%u,%u,%u,%u,%u (want 0,0,3,0,3,0)",
         pkt->_unk1, pkt->_unk2, pkt->_unk3, pkt->_unk4, pkt->_unk5, pkt->_unk6);
    return false;
  }

  out->addr         = pkt->addr;
  out->addrType     = pkt->addr_type;
  out->aircraftType = pkt->aircraft_type;
  out->stealth      = pkt->stealth;
  out->noTrack      = pkt->no_track;
  out->onGround     = (pkt->airborne == 1);
  out->protoVersion = 7;

  out->altitude = (float)(descale(pkt->alt, 12, 1) - 1000); // WGS84 ellipsoid

  int32_t round_lat = (int32_t)(refLat * 1e7) / 52;
  int32_t lat = (pkt->lat - round_lat) % (uint32_t)0x100000;
  if (lat >= 0x080000) lat -= 0x100000;
  lat = ((lat + round_lat) * 52);
  out->lat = (float)lat / 1e7f;

  int ilat = (int)fabsf(out->lat);
  if (ilat > 89) ilat = 89;
  int32_t lon_div = (ilat < 14) ? 52 : lon_div_table[ilat - 14];

  int32_t round_lon = (int32_t)(refLon * 1e7) / lon_div;
  int32_t lon = (pkt->lon - round_lon) % (uint32_t)0x100000;
  if (lon >= 0x080000) lon -= 0x100000;
  lon = ((lon + round_lon) * lon_div);
  out->lon = (float)lon / 1e7f;

  out->speed  = (float)(uint16_t)descale(pkt->hs, 8, 2) * 3.6f / 10.0f;
  out->climb  = (float)(int16_t)descale(pkt->vs, 6, 2) / 10.0f;
  out->course = (float)pkt->course / 2.0f;
  return true;
}

bool flarm_decode_payload(const uint8_t* payload, float refLat, float refLon, time_t nowUtc, FlarmTarget* out) {
  // The type nibble sits in the clear: byte 3 belongs to the first word, which
  // neither generation encrypts.
  uint8_t type = payload[3] & 0x0F;

  flarm_debug_note[0] = '\0';

  if (type != 0 && type != 2) {
    snprintf(flarm_debug_note, sizeof(flarm_debug_note),
             "unknown protocol: byte3 low nibble=%u (want 0 for v6, 2 for v7)", type);
    return false;
  }

  // The key schedule of both generations depends on the transmit second, which
  // we only know approximately. Try the neighbouring seconds like GXAirCom does.
  static const int8_t offsets[] = { 0, 1, -1, 2, -2 };

  for (size_t i = 0; i < sizeof(offsets); i++) {
    // The decoders decrypt in place, so every attempt needs a fresh copy.
    uint32_t aligned[FLARM_PAYLOAD_LEN / 4 + 1];
    uint8_t* work = (uint8_t*)aligned;
    memcpy(work, payload, FLARM_PAYLOAD_LEN);

    uint32_t ts = (uint32_t)(nowUtc + offsets[i]);
    note_enabled = (offsets[i] == 0);
    bool ok = (type == 2) ? decode_v7(work, refLat, refLon, ts, out)
                          : decode_v6(work, refLat, refLon, ts, out);
    note_enabled = false;

    if (!ok) continue;

    // A wrong timestamp usually still passes the field checks but throws the
    // position far off, so treat distant targets as a failed decode.
    int km = distance(refLat, refLon, out->lat, out->lon);
    if (km > 100) {
      if (offsets[i] == 0) {
        snprintf(flarm_debug_note, sizeof(flarm_debug_note),
                 "decoded but %d km away (addr %06X, %.5f/%.5f) - rejected as implausible",
                 km, (unsigned)out->addr, out->lat, out->lon);
      }
      continue;
    }

    return true;
  }

  return false;
}
