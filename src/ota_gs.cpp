#include "ota_gs.h"
#include "config_gs.h"

#if BREEZEDUDE_GS_ENABLE_LORA_OTA

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <RadioLib.h>
#include <memory>
#include <new>
#include <vector>
#include <math.h>

#include "helper.h"

extern volatile bool receivedFlag;
extern void webconsole_print(String in);
extern void ota_status_v(const String& msg);
extern void ota_progress_start(uint32_t totalChunks);
extern void ota_progress_chunk(uint16_t seq, uint8_t retries, float snr, uint8_t ackResendCount,
                                int8_t deviceSnr, int8_t deviceRssi, uint8_t proactiveAckResendCount);
extern void ota_progress_end(bool success);

#ifndef OTA_GS_BACKEND_URL
  #define OTA_GS_BACKEND_URL "http://fanet.breezedude.de/api/ota/check"
#endif
#ifndef OTA_GS_STATS_URL
  #define OTA_GS_STATS_URL "http://fanet.breezedude.de/api/ota/stats"
#endif
#ifndef OTA_GS_FAST_BW_KHZ
  #define OTA_GS_FAST_BW_KHZ 500
#endif
#ifndef OTA_GS_FAST_SF
  #define OTA_GS_FAST_SF 7
#endif
#ifndef OTA_GS_FAST_CR
  #define OTA_GS_FAST_CR 5
#endif
#ifndef OTA_GS_FAST_PREAMBLE
  #define OTA_GS_FAST_PREAMBLE 6
#endif
#ifndef OTA_GS_FAST_SYNCWORD
  #define OTA_GS_FAST_SYNCWORD 0x12
#endif
#ifndef OTA_GS_FAST_FREQ_MHZ
  #define OTA_GS_FAST_FREQ_MHZ 868.2f
#endif

static constexpr uint8_t FANET_ACK_TYPE = 0x00u;
static constexpr uint8_t OTA_DEBUG_TYPE = 0x02u;
static constexpr uint8_t OTA_MAGIC0 = 'O';
static constexpr uint8_t OTA_MAGIC1 = 'T';
static constexpr uint8_t OTA_PROTO = 4u;
static constexpr uint8_t OTA_OP_START = 0x01u;
static constexpr uint8_t OTA_OP_CHUNK = 0x02u;
static constexpr uint8_t OTA_OP_FINISH = 0x03u;
static constexpr uint8_t OTA_OP_ACK = 0x04u;
static constexpr uint8_t OTA_OP_ABORT = 0x05u;
static constexpr uint8_t OTA_STATE_IDLE = 0x00u;
static constexpr uint8_t OTA_STATE_RX = 0x02u;
static constexpr uint8_t OTA_STATE_READY = 0x03u;
static constexpr uint8_t OTA_ERR_SIZE = 0x82u;
static constexpr uint8_t OTA_ERR_TIMEOUT = 0x87u;
static constexpr uint16_t OTA_MAX_CHUNK = 192u;
static constexpr uint32_t OTA_GS_ACK_TIMEOUT_MS = 4000UL;
static constexpr uint16_t OTA_GS_POST_ACK_DELAY_MS = 35u;
static constexpr uint16_t OTA_GS_RETRY_BACKOFF_MS = 80u;
static constexpr uint8_t OTA_GS_CHUNK_MAX_ATTEMPTS = 15u;

// Per-chunk transfer stats are reported as two parallel 4-bit-per-chunk
// nibble arrays (2 chunks packed per byte, high nibble = first chunk of the
// pair, low nibble = second; if the chunk count is odd the trailing low
// nibble is padding):
//  - retries: 0..14 = accepted on attempt N+1, 15 = this chunk caused the
//    transfer to be aborted.
//  - snr: bucket 0..15 mapping linearly onto [snrMinDb..snrMaxDb] (sent as
//    two leading signed int8 header bytes), as seen by the GS for the
//    chunk's ack.
static constexpr uint8_t OTA_GS_STAT_RETRY_ABORT = 15u; // 4-bit field max (2^4 - 1)
static constexpr uint8_t OTA_GS_STAT_SNR_BUCKETS = 16u; // 4-bit field range (2^4)

struct __attribute__((packed)) ota_pkt_prefix_t {
  uint8_t magic0;
  uint8_t magic1;
  uint8_t proto;
  uint8_t op;
};

struct __attribute__((packed)) ota_offer_pkt_t {
  ota_pkt_prefix_t prefix;
  uint32_t nonce;
  uint16_t target_version_bcd;
  uint16_t chunk_size;
  uint32_t image_size;
  uint32_t image_crc32;
  uint32_t expires;
  uint8_t image_sha256[32];
  uint8_t signature_raw[64];
};

struct __attribute__((packed)) ota_chunk_pkt_t {
  ota_pkt_prefix_t prefix;
  uint16_t seq;
  uint16_t data_len;
  uint8_t payload[OTA_MAX_CHUNK];
};

struct __attribute__((packed)) ota_finish_pkt_t {
  ota_pkt_prefix_t prefix;
  uint32_t nonce;
  uint32_t image_size;
  uint32_t image_crc32;
};

struct __attribute__((packed)) ota_abort_pkt_t {
  ota_pkt_prefix_t prefix;
  uint32_t nonce;
  uint8_t status;
  uint8_t reserved[3];
};

struct __attribute__((packed)) ota_ack_pkt_t {
  ota_pkt_prefix_t prefix;
  uint32_t nonce;
  uint16_t next_seq;
  uint8_t status;
  uint8_t accepted_chunk;
  uint8_t ack_resend_count; // # of times the device (re)sent this ack due to duplicate chunks
  int8_t chunk_snr; // SNR (dB) of the most recently received chunk, as measured by the device
  int8_t chunk_rssi; // RSSI (dBm) of the most recently received chunk, as measured by the device
  uint8_t proactive_ack_resend_count; // subset of ack_resend_count sent proactively (no dup chunk seen yet)
};

struct OtaAnnouncement {
  uint8_t vendor = 0;
  uint16_t address = 0;
  uint16_t versionBcd = 0;
  uint32_t nonce = 0;
  uint16_t slotCapacityKb = 0;
  uint8_t otaProto = OTA_PROTO;
  uint8_t otaState = 0;
  int rssi = 0;   // RSSI of received announcement packet
};

struct OtaManifest {
  bool available = false;
  bool suppressNoUpdateNotify = false;
  String noUpdateReason;
  String firmwareUrl;
  String fileName;
  String slotVariant;
  uint16_t targetVersionBcd = 0;
  uint32_t imageSize = 0;
  uint32_t imageCrc32 = 0;
  String imageSha256;
  uint8_t imageSha256Raw[32] = {0};
  uint8_t signatureRaw[64] = {0};
  uint32_t expires = 0;
  uint16_t chunkSize = OTA_MAX_CHUNK;
};

static bool s_busy = false;
static uint32_t s_last_nonce = 0;
static uint32_t s_last_attempt_ms = 0;

static void ota_status(const String& msg) {
  Serial.println(msg);
  webconsole_print(msg);
}

static uint16_t readLe16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t readLe32(const uint8_t* p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static String toHex(const uint8_t* data, size_t len) {
  static const char hexmap[] = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for(size_t i = 0; i < len; ++i) {
    out += hexmap[(data[i] >> 4) & 0x0F];
    out += hexmap[data[i] & 0x0F];
  }
  return out;
}

static String formatVersionBcd(uint16_t bcd) {
  const uint8_t major = (uint8_t)((bcd >> 8) & 0x0Fu);
  const uint8_t minor = (uint8_t)((bcd >> 4) & 0x0Fu);
  const uint8_t patch = (uint8_t)(bcd & 0x0Fu);
  return String(major) + "." + String(minor) + "." + String(patch);
}

// Maps an SNR reading (dB) to a 4-bit bucket covering snrMinDb..snrMaxDb,
// clamping out-of-range values to the nearest bucket. Returns bucket 0 if
// the range is degenerate (snrMaxDb <= snrMinDb).
static uint8_t encodeSnrBucket(float snrDb, float snrMinDb, float snrMaxDb) {
  if(snrMaxDb <= snrMinDb) {
    return 0u;
  }
  long bucket = lroundf((snrDb - snrMinDb) / (snrMaxDb - snrMinDb) * (float)(OTA_GS_STAT_SNR_BUCKETS - 1u));
  if(bucket < 0) {
    bucket = 0;
  } else if(bucket > (long)(OTA_GS_STAT_SNR_BUCKETS - 1u)) {
    bucket = (long)(OTA_GS_STAT_SNR_BUCKETS - 1u);
  }
  return (uint8_t)bucket;
}

// Packs two 4-bit values (0..15) into one byte, hi in bits 7-4, lo in bits 3-0.
static uint8_t packNibbles(uint8_t hi, uint8_t lo) {
  return (uint8_t)(((hi & 0x0Fu) << 4) | (lo & 0x0Fu));
}

// Packs a vector of 4-bit values (0..15) two per byte, padding a trailing
// odd value's low nibble with 0.
static std::vector<uint8_t> packNibbleArray(const std::vector<uint8_t>& values) {
  std::vector<uint8_t> out;
  out.reserve((values.size() + 1u) / 2u);
  for(size_t i = 0; i < values.size(); i += 2u) {
    const uint8_t hi = values[i];
    const uint8_t lo = (i + 1u < values.size()) ? values[i + 1u] : 0u;
    out.push_back(packNibbles(hi, lo));
  }
  return out;
}

static bool parseDebugType4(const hwInfoData& info, OtaAnnouncement& ann) {
  if(info.rawLen < 10u || info.rawAfterBuildDate[0] != OTA_DEBUG_TYPE) {
    return false;
  }

  ann.vendor = info.vid;
  ann.address = info.fanet_id;
  ann.versionBcd = readLe16(&info.rawAfterBuildDate[1]);
  ann.nonce = readLe32(&info.rawAfterBuildDate[3]);
  ann.slotCapacityKb = readLe16(&info.rawAfterBuildDate[7]);
  ann.otaProto = info.rawAfterBuildDate[9];
  ann.otaState = (info.rawLen >= 11u) ? info.rawAfterBuildDate[10] : 0u;
  ann.rssi = (int)info.rssi;
  return true;
}

static bool parseHexBytes(const String& hex, uint8_t* out, size_t outLen) {
  if(hex.length() != (int)(outLen * 2u)) {
    return false;
  }

  auto fromNibble = [](char c) -> int {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if(c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };

  for(size_t i = 0; i < outLen; ++i) {
    const int hi = fromNibble(hex[(int)(i * 2u)]);
    const int lo = fromNibble(hex[(int)(i * 2u + 1u)]);
    if(hi < 0 || lo < 0) {
      return false;
    }
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static String ota_force_https_url(String url) {
  if(url.startsWith("http://")) {
    url = "https://" + url.substring(7);
  }
  return url;
}

static bool queryBackend(const OtaAnnouncement& ann, OtaManifest& manifest) {
  const String url = String(OTA_GS_BACKEND_URL) +
                     "?vendor=" + String(ann.vendor) +
                     "&address=" + String(ann.address) +
                     "&nonce=" + String(ann.nonce) +
                     "&versionBcd=" + String(ann.versionBcd) +
                     "&slotCapacityKb=" + String(ann.slotCapacityKb) +
                     "&rssi=" + String(ann.rssi);

  //webconsole_print("OTA: Querying backend: " + url);
  //webconsole_print("OTA: Querying backend for updates");

  // Clients must outlive http.getString(), so declare at function scope.
  WiFiClient plainClient;
  WiFiClientSecure secureClient;

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  int code = -1;

  if(url.startsWith("https://")) {
    secureClient.setInsecure();
    secureClient.setTimeout(15000);
    if(!http.begin(secureClient, url)) {
      ota_status("OTA: backend begin failed");
      return false;
    }
  } else {
    plainClient.setTimeout(15000);
    if(!http.begin(plainClient, url)) {
      ota_status("OTA: backend begin failed");
      return false;
    }
  }
  code = http.GET();

  if(code != HTTP_CODE_OK) {
    ota_status(String("OTA: backend HTTP error ") + code);
    http.end();
    return false;
  }

  String responseBody = http.getString();
  //webconsole_print("OTA: Backend response (" + String(responseBody.length()) + " bytes): " + responseBody.substring(0, min(200, (int)responseBody.length())));
  
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, responseBody);
  http.end();
  if(err) {
    ota_status(String("OTA: JSON parse failed: ") + err.c_str());
    webconsole_print("OTA: Full response: " + responseBody);
    return false;
  }

  manifest.available = doc["available"] | false;
  if(!manifest.available) {
    const char* reason = doc["reason"] | "";
    manifest.noUpdateReason = reason;

    String reasonLower = manifest.noUpdateReason;
    reasonLower.toLowerCase();
    if(reasonLower.indexOf("another ground station") >= 0 ||
       reasonLower.indexOf("better signal") >= 0) {
      manifest.suppressNoUpdateNotify = true;
      webconsole_print("OTA: Other GS with better signal will handle update");
    }

    ota_status("OTA: backend reports no update available");
    return true;
  }

  const char* firmwareUrl = doc["firmwareUrl"] | "";
  const char* fileName = doc["fileName"] | "";
  const char* slotVariant = doc["slotVariant"] | "";
  const char* imageSha256 = doc["imageSha256"] | "";
  const char* signatureRawHexCstr = doc["signatureRawHex"] | "";

  manifest.firmwareUrl = firmwareUrl;
  manifest.fileName = fileName;
  manifest.slotVariant = slotVariant;
  manifest.targetVersionBcd = doc["targetVersionBcd"] | (uint16_t)0u;
  manifest.imageSize = doc["imageSize"] | 0u;
  manifest.imageCrc32 = doc["imageCrc32"] | 0u;
  manifest.imageSha256 = imageSha256;
  manifest.expires = doc["expires"] | 0u;
  manifest.chunkSize = min((uint16_t)(doc["chunkSize"] | (uint16_t)OTA_MAX_CHUNK), (uint16_t)OTA_MAX_CHUNK);
  if(manifest.chunkSize < 32u) {
    manifest.chunkSize = OTA_MAX_CHUNK;
  }

  if(manifest.firmwareUrl.isEmpty() || manifest.imageSize == 0u || manifest.imageSha256.length() != 64u || strlen(signatureRawHexCstr) != 128u) {
    ota_status("OTA: backend manifest incomplete");
    manifest.available = false;
    return false;
  }

  const String signatureRawHex(signatureRawHexCstr);
  if(!parseHexBytes(manifest.imageSha256, manifest.imageSha256Raw, sizeof(manifest.imageSha256Raw)) ||
     !parseHexBytes(signatureRawHex, manifest.signatureRaw, sizeof(manifest.signatureRaw))) {
    ota_status("OTA: signed manifest payload invalid");
    manifest.available = false;
    return false;
  }

  const uint32_t nowTs = (uint32_t)time(nullptr);
  if(manifest.expires && nowTs && nowTs > manifest.expires) {
    ota_status("OTA: manifest expired");
    manifest.available = false;
    return false;
  }

  ota_status(String("OTA: signed manifest ok, crc32=0x") + String(manifest.imageCrc32, HEX));
  return true;
}

static void applyRadioMode(bool fastMode) {
  configure_lora_radio(fastMode);
}

static bool sendFanetAck(const OtaAnnouncement& ann) {
  fanet_header ack = {};
  ack.type = FANET_ACK_TYPE;
  ack.forward = false;
  ack.ext_header = false;
  ack.vendor = ann.vendor;
  ack.address = ann.address;
  int state = radio_phy->transmit((uint8_t*)&ack, sizeof(ack));
  if(state != RADIOLIB_ERR_NONE) {
    ota_status(String("OTA: FANET ACK send failed: ") + state);
    return false;
  }
  return true;
}

static void fillPrefix(ota_pkt_prefix_t& prefix, uint8_t op) {
  prefix.magic0 = OTA_MAGIC0;
  prefix.magic1 = OTA_MAGIC1;
  prefix.proto = OTA_PROTO;
  prefix.op = op;
}

static bool transmitPacket(const uint8_t* data, size_t len) {
  int state = radio_phy->transmit((uint8_t*)data, len);
  if(state != RADIOLIB_ERR_NONE) {
    ota_status(String("OTA: radio TX failed: ") + state);
    return false;
  }
  receivedFlag = false;
  radio_phy->startReceive();
  return true;
}

static bool waitForPacket(uint8_t* packet, size_t& outLen, uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  while(millis() < deadline) {
    if(receivedFlag) {
      receivedFlag = false;
      int numBytes = radio_phy->getPacketLength();
      if(numBytes <= 0 || numBytes > 255) {
        radio_phy->startReceive();
        continue;
      }
      int state = radio_phy->readData(packet, numBytes);
      if(state == RADIOLIB_ERR_NONE) {
        outLen = (size_t)numBytes;
        return true;
      }
      radio_phy->startReceive();
    }
    delay(1);
  }
  return false;
}

// Info extracted from a device ACK packet (OTA protocol v4). chunkSnr/chunkRssi
// are the SNR (dB) / RSSI (dBm) of the most recent chunk packet as measured by
// the device, and proactiveAckResendCount is the subset of ackResendCount that
// were proactive resends (no duplicate chunk seen yet) - see ota_lora.cpp.
struct OtaAckInfo {
  uint16_t nextSeq = 0;
  uint8_t status = 0;
  uint8_t ackResendCount = 0;
  int8_t chunkSnr = 0;
  int8_t chunkRssi = 0;
  uint8_t proactiveAckResendCount = 0;
};

static bool parseOtaAck(const uint8_t* packet, size_t len, uint32_t nonce, OtaAckInfo& info) {
  if(len < sizeof(ota_ack_pkt_t)) {
    return false;
  }

  const ota_ack_pkt_t* ack = (const ota_ack_pkt_t*)packet;
  if(ack->prefix.magic0 != OTA_MAGIC0 || ack->prefix.magic1 != OTA_MAGIC1 || ack->prefix.proto != OTA_PROTO || ack->prefix.op != OTA_OP_ACK) {
    return false;
  }
  if(ack->nonce != nonce) {
    return false;
  }

  info.nextSeq = ack->next_seq;
  info.status = ack->status;
  info.ackResendCount = ack->ack_resend_count;
  info.chunkSnr = ack->chunk_snr;
  info.chunkRssi = ack->chunk_rssi;
  info.proactiveAckResendCount = ack->proactive_ack_resend_count;
  return true;
}

static bool waitForAck(uint32_t nonce, OtaAckInfo& info, uint32_t timeoutMs) {
  uint8_t packet[255] = {0};
  size_t len = 0;
  while(waitForPacket(packet, len, timeoutMs)) {
    if(parseOtaAck(packet, len, nonce, info)) {
      return true;
    }
    len = 0;
  }
  return false;
}

static bool sendStart(const OtaAnnouncement& ann, const OtaManifest& manifest) {
  ota_offer_pkt_t pkt = {};
  fillPrefix(pkt.prefix, OTA_OP_START);
  pkt.nonce = ann.nonce;
  pkt.target_version_bcd = manifest.targetVersionBcd;
  pkt.chunk_size = manifest.chunkSize;
  pkt.image_size = manifest.imageSize;
  pkt.image_crc32 = manifest.imageCrc32;
  pkt.expires = manifest.expires;
  memcpy(pkt.image_sha256, manifest.imageSha256Raw, sizeof(pkt.image_sha256));
  memcpy(pkt.signature_raw, manifest.signatureRaw, sizeof(pkt.signature_raw));
  return transmitPacket((const uint8_t*)&pkt, sizeof(pkt));
}

static bool sendFinish(const OtaAnnouncement& ann, const OtaManifest& manifest) {
  ota_finish_pkt_t pkt = {};
  fillPrefix(pkt.prefix, OTA_OP_FINISH);
  pkt.nonce = ann.nonce;
  pkt.image_size = manifest.imageSize;
  pkt.image_crc32 = manifest.imageCrc32;
  return transmitPacket((const uint8_t*)&pkt, sizeof(pkt));
}

static bool sendAbort(const OtaAnnouncement& ann, uint8_t status) {
  ota_abort_pkt_t pkt = {};
  fillPrefix(pkt.prefix, OTA_OP_ABORT);
  pkt.nonce = ann.nonce;
  pkt.status = status;
  return transmitPacket((const uint8_t*)&pkt, sizeof(pkt));
}

static void notifyNoUpdate(const OtaAnnouncement& ann) {
  //ota_status("OTA: no update available, notifying device");
  if(!sendFanetAck(ann)) {
    return;
  }
  delay(80);
  applyRadioMode(true);
  delay(120);
  sendAbort(ann, OTA_STATE_IDLE);
  applyRadioMode(false);
}

static bool downloadFirmwareImage(const OtaManifest& manifest, std::unique_ptr<uint8_t[]>& firmwareImage) {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  int code = -1;

  const String& firmwareUrl = manifest.firmwareUrl;
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  secureClient.setTimeout(20000);
  const bool useHttps = firmwareUrl.startsWith("https://");
  if(!http.begin(useHttps ? static_cast<WiFiClient&>(secureClient) : plainClient, firmwareUrl)) {
    ota_status("OTA: firmware download begin failed");
    return false;
  }

  code = http.GET();
  if(code != HTTP_CODE_OK) {
    ota_status(String("OTA: firmware download HTTP error ") + code);
    http.end();
    return false;
  }

  firmwareImage.reset(new (std::nothrow) uint8_t[manifest.imageSize]);
  if(!firmwareImage) {
    ota_status("OTA: insufficient RAM for firmware buffer");
    http.end();
    return false;
  }

  ota_status(String("OTA: preloading firmware, ") + manifest.imageSize + " bytes");
  WiFiClient* stream = http.getStreamPtr();
  size_t received = 0;
  uint8_t lastProgress = 0;
  uint32_t stallDeadline = millis() + 15000UL;

  while(received < manifest.imageSize) {
    if(stream->available()) {
      const size_t want = min((size_t)4096u, (size_t)(manifest.imageSize - received));
      const int got = stream->read(&firmwareImage[received], (int)want);
      if(got > 0) {
        received += (size_t)got;
        stallDeadline = millis() + 15000UL;
        const uint8_t progress = manifest.imageSize ? (uint8_t)((received * 100u) / manifest.imageSize) : 100u;
        if(progress >= (uint8_t)(lastProgress + 25u) || received == manifest.imageSize) {
          lastProgress = progress;
          ota_status(String("OTA: preload ") + progress + "% (" + received + "/" + manifest.imageSize + " bytes)");
        }
      }
    } else if(millis() > stallDeadline) {
      break;
    } else {
      delay(5);
    }
  }

  http.end();
  if(received != manifest.imageSize) {
    ota_status(String("OTA: firmware preload incomplete ") + received + "/" + manifest.imageSize + " bytes");
    firmwareImage.reset();
    return false;
  }

  return true;
}

// Streams the firmware image starting at chunk `startSeq`, ACK-ing each chunk
// before sending the next. On success or failure, appends one entry per
// attempted chunk to `chunkRetries` (retry count, or OTA_GS_STAT_RETRY_ABORT
// if this chunk caused the transfer to be aborted), `chunkSnr` (raw SNR in
// dB, as measured by the GS for the device's ACK) and `chunkAckResends` (the
// device's ack_resend_count for the accepted ack, i.e. how many times the
// device had to resend its ack for this chunk before the GS received it - 0
// if no ack ever arrived). Additionally appends `chunkDeviceSnr`/
// `chunkDeviceRssi` (SNR/RSSI of the chunk packet itself, as measured by the
// device) and `chunkProactiveAckResends` (subset of the ack-resend count that
// were proactive, i.e. sent before the device saw a duplicate chunk), so the
// caller can report transfer quality in both directions to the backend
// regardless of outcome.
static bool streamFirmware(const OtaAnnouncement& ann, const OtaManifest& manifest, const uint8_t* firmwareImage,
                            uint16_t startSeq, std::vector<uint8_t>& chunkRetries, std::vector<float>& chunkSnr,
                            std::vector<uint8_t>& chunkAckResends, std::vector<int8_t>& chunkDeviceSnr,
                            std::vector<int8_t>& chunkDeviceRssi, std::vector<uint8_t>& chunkProactiveAckResends) {
  if(!firmwareImage) {
    ota_status("OTA: no buffered firmware image");
    return false;
  }

  //ota_status(String("OTA: transfer started, ") + manifest.imageSize + " bytes");

  const uint32_t transferStartMs = millis();
  uint8_t chunk[OTA_MAX_CHUNK] = {0};
  uint16_t seq = startSeq;
  uint32_t sent = (uint32_t)startSeq * manifest.chunkSize;
  uint8_t lastProgress = 0;

  while(sent < manifest.imageSize) {
    const size_t want = min((size_t)manifest.chunkSize, (size_t)(manifest.imageSize - sent));
    memcpy(chunk, &firmwareImage[sent], want);
    const size_t got = want;

    ota_chunk_pkt_t pkt = {};
    fillPrefix(pkt.prefix, OTA_OP_CHUNK);
    pkt.seq = seq;
    pkt.data_len = (uint16_t)got;
    memcpy(pkt.payload, chunk, got);

    bool accepted = false;
    uint8_t retries = 0;
    uint8_t ackResendCount = 0;
    uint8_t proactiveAckResendCount = 0;
    int8_t deviceSnr = 0;
    int8_t deviceRssi = 0;
    float snr = -20.0f; // fallback if no ACK SNR was observed for this chunk
    for(uint8_t attempt = 0; attempt < OTA_GS_CHUNK_MAX_ATTEMPTS && !accepted; ++attempt) {
      if(attempt > 0u) {
        delay((uint32_t)OTA_GS_RETRY_BACKOFF_MS * attempt);
      }

      if(!transmitPacket((const uint8_t*)&pkt, sizeof(ota_pkt_prefix_t) + 4u + got)) {
        continue;
      }

      OtaAckInfo ackInfo;
      if(waitForAck(ann.nonce, ackInfo, OTA_GS_ACK_TIMEOUT_MS)) {
        snr = radio_phy->getSNR();
        if(ackInfo.status == OTA_STATE_RX && ackInfo.nextSeq >= (uint16_t)(seq + 1u)) {
          accepted = true;
          retries = attempt;
          ackResendCount = ackInfo.ackResendCount;
          proactiveAckResendCount = ackInfo.proactiveAckResendCount;
          deviceSnr = ackInfo.chunkSnr;
          deviceRssi = ackInfo.chunkRssi;
          delay(OTA_GS_POST_ACK_DELAY_MS);
        } else if(ackInfo.status == OTA_ERR_SIZE) {
          ota_status(String("OTA: device rejected size at seq ") + seq);
          chunkRetries.push_back(OTA_GS_STAT_RETRY_ABORT);
          chunkSnr.push_back(snr);
          chunkAckResends.push_back(0);
          chunkDeviceSnr.push_back(0);
          chunkDeviceRssi.push_back(0);
          chunkProactiveAckResends.push_back(0);
          ota_progress_chunk(seq, OTA_GS_STAT_RETRY_ABORT, snr, 0, 0, 0, 0);
          return false;
        } else {
          ota_status_v(String("OTA: ack mismatch seq ") + seq + " status=0x" + String(ackInfo.status, HEX) + " next=" + ackInfo.nextSeq);
        }
      } else {
        ota_status_v(String("OTA: ack timeout seq ") + seq + " try=" + (attempt + 1u) + " t=" + String((millis() - transferStartMs) / 1000.0f, 2) + "s");
      }
    }

    if(!accepted) {
      ota_status(String("OTA: chunk retransmit failed at seq ") + seq + " after " + String((millis() - transferStartMs) / 1000.0f, 2) + " s");
      chunkRetries.push_back(OTA_GS_STAT_RETRY_ABORT);
      chunkSnr.push_back(snr);
      chunkAckResends.push_back(0);
      chunkDeviceSnr.push_back(0);
      chunkDeviceRssi.push_back(0);
      chunkProactiveAckResends.push_back(0);
      ota_progress_chunk(seq, OTA_GS_STAT_RETRY_ABORT, snr, 0, 0, 0, 0);
      return false;
    }

    chunkRetries.push_back(retries);
    chunkSnr.push_back(snr);
    chunkAckResends.push_back(ackResendCount);
    chunkDeviceSnr.push_back(deviceSnr);
    chunkDeviceRssi.push_back(deviceRssi);
    chunkProactiveAckResends.push_back(proactiveAckResendCount);
    ota_progress_chunk(seq, retries, snr, ackResendCount, deviceSnr, deviceRssi, proactiveAckResendCount);
    ota_status_v(String("OTA: chunk seq ") + seq + " ok, retries=" + retries + " snr=" + String(snr, 1) +
                  " ackResends=" + ackResendCount + " (proactive=" + proactiveAckResendCount + ")" +
                  " deviceSnr=" + deviceSnr + "dB deviceRssi=" + deviceRssi + "dBm");
    sent += got;
    seq++;

    uint8_t progress = manifest.imageSize ? (uint8_t)((sent * 100u) / manifest.imageSize) : 100u;
    if(progress >= (uint8_t)(lastProgress + 10u) || sent == manifest.imageSize) {
      lastProgress = progress;
      ota_status(String("OTA: transfer ") + progress + "% (" + sent + "/" + manifest.imageSize + " bytes, t=" + String((millis() - transferStartMs) / 1000.0f, 2) + " s)");
    }
  }

  return sent == manifest.imageSize;
}

// Posts the per-chunk transfer stats collected by streamFirmware() to the
// backend as a raw binary blob (Content-Type: application/octet-stream),
// with transfer metadata as query parameters. `startSeq` is the first chunk
// covered by `chunkRetries`/`chunkSnr` - currently always 0, but kept as an
// explicit field so a future resumed transfer (continuing after the last
// successfully acked chunk instead of restarting at 0) doesn't require a
// payload format change. Best-effort: failures are logged but never abort
// the OTA flow.
//
// Payload layout:
//   [snrMinDb int8][snrMaxDb int8]
//   [deviceSnrMinDb int8][deviceSnrMaxDb int8]
//   [deviceRssiMinDbm int8][deviceRssiMaxDbm int8]
//   [retries nibble array]
//   [snr nibble array]               (GS-measured SNR of the device's ACK)
//   [ack-resends nibble array]
//   [device snr nibble array]        (device-measured SNR of the chunk)
//   [device rssi nibble array]       (device-measured RSSI of the chunk)
//   [proactive ack-resends nibble array]
// All nibble arrays hold one 4-bit entry per chunk (see OTA_GS_STAT_* above),
// 2 chunks packed per byte. `statCount` (= chunkRetries.size()) tells the
// backend how many chunk entries the nibble arrays hold, i.e. each array is
// ceil(statCount / 2) bytes long.
static void postOtaStats(const OtaAnnouncement& ann, const OtaManifest& manifest,
                          const std::vector<uint8_t>& chunkRetries, const std::vector<float>& chunkSnr,
                          const std::vector<uint8_t>& chunkAckResends,
                          const std::vector<int8_t>& chunkDeviceSnr, const std::vector<int8_t>& chunkDeviceRssi,
                          const std::vector<uint8_t>& chunkProactiveAckResends,
                          uint16_t startSeq, bool success) {
  if(chunkRetries.empty()) {
    return;
  }

  const uint32_t totalChunks = manifest.chunkSize ? (manifest.imageSize + manifest.chunkSize - 1u) / manifest.chunkSize : 0u;
  const int32_t abortSeq = success ? -1 : (int32_t)startSeq + (int32_t)chunkRetries.size() - 1;

  float snrMinDb = chunkSnr.front();
  float snrMaxDb = chunkSnr.front();
  for(float snr : chunkSnr) {
    snrMinDb = min(snrMinDb, snr);
    snrMaxDb = max(snrMaxDb, snr);
  }
  const int8_t snrMinDbInt = (int8_t)constrain((long)floorf(snrMinDb), -128L, 127L);
  const int8_t snrMaxDbInt = (int8_t)constrain((long)ceilf(snrMaxDb), -128L, 127L);

  int8_t deviceSnrMinDb = chunkDeviceSnr.front();
  int8_t deviceSnrMaxDb = chunkDeviceSnr.front();
  for(int8_t snr : chunkDeviceSnr) {
    deviceSnrMinDb = min(deviceSnrMinDb, snr);
    deviceSnrMaxDb = max(deviceSnrMaxDb, snr);
  }

  int8_t deviceRssiMinDbm = chunkDeviceRssi.front();
  int8_t deviceRssiMaxDbm = chunkDeviceRssi.front();
  for(int8_t rssi : chunkDeviceRssi) {
    deviceRssiMinDbm = min(deviceRssiMinDbm, rssi);
    deviceRssiMaxDbm = max(deviceRssiMaxDbm, rssi);
  }

  std::vector<uint8_t> snrBuckets;
  snrBuckets.reserve(chunkSnr.size());
  for(float snr : chunkSnr) {
    snrBuckets.push_back(encodeSnrBucket(snr, (float)snrMinDbInt, (float)snrMaxDbInt));
  }

  std::vector<uint8_t> ackResendNibbles;
  ackResendNibbles.reserve(chunkAckResends.size());
  for(uint8_t ackResendCount : chunkAckResends) {
    ackResendNibbles.push_back(min(ackResendCount, (uint8_t)OTA_GS_STAT_RETRY_ABORT));
  }

  std::vector<uint8_t> deviceSnrBuckets;
  deviceSnrBuckets.reserve(chunkDeviceSnr.size());
  for(int8_t snr : chunkDeviceSnr) {
    deviceSnrBuckets.push_back(encodeSnrBucket((float)snr, (float)deviceSnrMinDb, (float)deviceSnrMaxDb));
  }

  std::vector<uint8_t> deviceRssiBuckets;
  deviceRssiBuckets.reserve(chunkDeviceRssi.size());
  for(int8_t rssi : chunkDeviceRssi) {
    deviceRssiBuckets.push_back(encodeSnrBucket((float)rssi, (float)deviceRssiMinDbm, (float)deviceRssiMaxDbm));
  }

  std::vector<uint8_t> proactiveAckResendNibbles;
  proactiveAckResendNibbles.reserve(chunkProactiveAckResends.size());
  for(uint8_t proactiveCount : chunkProactiveAckResends) {
    proactiveAckResendNibbles.push_back(min(proactiveCount, (uint8_t)OTA_GS_STAT_RETRY_ABORT));
  }

  std::vector<uint8_t> payload;
  payload.push_back((uint8_t)snrMinDbInt);
  payload.push_back((uint8_t)snrMaxDbInt);
  payload.push_back((uint8_t)deviceSnrMinDb);
  payload.push_back((uint8_t)deviceSnrMaxDb);
  payload.push_back((uint8_t)deviceRssiMinDbm);
  payload.push_back((uint8_t)deviceRssiMaxDbm);
  const std::vector<uint8_t> retryBytes = packNibbleArray(chunkRetries);
  const std::vector<uint8_t> snrBytes = packNibbleArray(snrBuckets);
  const std::vector<uint8_t> ackResendBytes = packNibbleArray(ackResendNibbles);
  const std::vector<uint8_t> deviceSnrBytes = packNibbleArray(deviceSnrBuckets);
  const std::vector<uint8_t> deviceRssiBytes = packNibbleArray(deviceRssiBuckets);
  const std::vector<uint8_t> proactiveAckResendBytes = packNibbleArray(proactiveAckResendNibbles);
  payload.insert(payload.end(), retryBytes.begin(), retryBytes.end());
  payload.insert(payload.end(), snrBytes.begin(), snrBytes.end());
  payload.insert(payload.end(), ackResendBytes.begin(), ackResendBytes.end());
  payload.insert(payload.end(), deviceSnrBytes.begin(), deviceSnrBytes.end());
  payload.insert(payload.end(), deviceRssiBytes.begin(), deviceRssiBytes.end());
  payload.insert(payload.end(), proactiveAckResendBytes.begin(), proactiveAckResendBytes.end());

  const String url = String(OTA_GS_STATS_URL) +
                     "?vendor=" + String(ann.vendor) +
                     "&address=" + String(ann.address) +
                     "&nonce=" + String(ann.nonce) +
                     "&targetVersionBcd=" + String(manifest.targetVersionBcd) +
                     "&imageSize=" + String(manifest.imageSize) +
                     "&chunkSize=" + String(manifest.chunkSize) +
                     "&totalChunks=" + String(totalChunks) +
                     "&startSeq=" + String(startSeq) +
                     "&statCount=" + String((uint32_t)chunkRetries.size()) +
                     "&success=" + String(success ? 1 : 0) +
                     "&abortSeq=" + String(abortSeq);

  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  bool began;
  if(url.startsWith("https://")) {
    secureClient.setInsecure();
    secureClient.setTimeout(10000);
    began = http.begin(secureClient, url);
  } else {
    plainClient.setTimeout(10000);
    began = http.begin(plainClient, url);
  }
  if(!began) {
    ota_status("OTA: stats upload begin failed");
    return;
  }

  http.addHeader("Content-Type", "application/octet-stream");
  const int code = http.POST(payload.data(), payload.size());
  if(code != HTTP_CODE_OK && code != HTTP_CODE_CREATED && code != HTTP_CODE_NO_CONTENT) {
    ota_status(String("OTA: stats upload HTTP error ") + code);
  }
  http.end();
}

void ota_gs_begin() {
  s_busy = false;
}

void ota_gs_try_update(const hwInfoData& info) {
  if(s_busy || WiFi.status() != WL_CONNECTED || info.vid == 0) {
    return;
  }

  OtaAnnouncement ann;
  if(!parseDebugType4(info, ann)) {
    return;
  }

  if(ann.nonce == s_last_nonce && (millis() - s_last_attempt_ms) < 30000UL) {
    return;
  }
  s_last_nonce = ann.nonce;
  s_last_attempt_ms = millis();

  OtaManifest manifest;
  s_busy = true;
  char deviceId[16];
  snprintf(deviceId, sizeof(deviceId), "%02X%04X", ann.vendor, ann.address);
  ota_status(String("OTA: request from ") + deviceId + " with version " + formatVersionBcd(ann.versionBcd));

  bool ok = queryBackend(ann, manifest);
  if(!ok) {
    s_busy = false;
    return;
  }
  if(!manifest.available) {
    if(manifest.suppressNoUpdateNotify) {
      //ota_status("OTA: passive mode, another ground station selected - no notify/abort sent");
      s_busy = false;
      return;
    }
    // Send config update FIRST while device is still in nonce RX mode (normal FANET settings, 15s window).
    // notifyNoUpdate() triggers fast RX mode on the device → after that the device is no longer
    // listening on normal FANET → config packet would be missed.
    config_gs_check_device(String(deviceId), ann.nonce);
    notifyNoUpdate(ann);
    s_busy = false;
    return;
  }

  ota_status(String("OTA: update available, ") + manifest.imageSize + " bytes");
  ota_status(String("OTA: installed ") + formatVersionBcd(ann.versionBcd) +
             " (0x" + String(ann.versionBcd, HEX) + ") -> target " +
             formatVersionBcd(manifest.targetVersionBcd) +
             " (0x" + String(manifest.targetVersionBcd, HEX) + ")");
  if(manifest.fileName.length()) {
    ota_status(String("OTA: selected firmware ") + manifest.fileName);
  } else {
    ota_status(String("OTA: download URL ") + manifest.firmwareUrl);
  }
  ota_status("OTA: backend signature received, device will verify");

  if(manifest.imageSize > ((uint32_t)ann.slotCapacityKb * 1024u)) {
    ota_status("OTA: image does not fit into target flash");
    s_busy = false;
    return;
  }

  std::unique_ptr<uint8_t[]> firmwareImage;
  if(!downloadFirmwareImage(manifest, firmwareImage)) {
    ota_status("OTA: firmware preload failed, aborting");
    s_busy = false;
    return;
  }
  ota_status("OTA: firmware buffered, starting target RX mode");

  ota_status("OTA: sending FANET ACK handshake");
  if(!sendFanetAck(ann)) {
    s_busy = false;
    return;
  }

  delay(80);
  applyRadioMode(true);
  delay(120);

  ota_status("OTA: switching to transfer mode and sending start packet");
  if(!sendStart(ann, manifest)) {
    applyRadioMode(false);
    s_busy = false;
    return;
  }

  OtaAckInfo ackInfo;
  //ota_status("OTA: waiting for device handshake response");
  if(!waitForAck(ann.nonce, ackInfo, 4000) || ackInfo.status != OTA_STATE_RX) {
    ota_status("OTA: start handshake not accepted by device");
    sendAbort(ann, OTA_ERR_TIMEOUT);
    applyRadioMode(false);
    s_busy = false;
    return;
  }

  ota_status("OTA: handshake successful, transferring firmware");
  const uint32_t transferStartMs = millis();
  // start_seq is currently always 0 (no resume support yet); kept explicit
  // so the stats payload format already accounts for a future resumed
  // transfer continuing from the last successfully acked chunk.
  const uint16_t startSeq = 0u;
  const uint32_t totalChunks = manifest.chunkSize ? (manifest.imageSize + manifest.chunkSize - 1u) / manifest.chunkSize : 0u;
  std::vector<uint8_t> chunkRetries;
  std::vector<float> chunkSnr;
  std::vector<uint8_t> chunkAckResends;
  std::vector<int8_t> chunkDeviceSnr;
  std::vector<int8_t> chunkDeviceRssi;
  std::vector<uint8_t> chunkProactiveAckResends;
  chunkRetries.reserve(totalChunks);
  chunkSnr.reserve(totalChunks);
  chunkAckResends.reserve(totalChunks);
  chunkDeviceSnr.reserve(totalChunks);
  chunkDeviceRssi.reserve(totalChunks);
  chunkProactiveAckResends.reserve(totalChunks);
  ota_progress_start(totalChunks);
  const bool transferOk = streamFirmware(ann, manifest, firmwareImage.get(), startSeq, chunkRetries, chunkSnr, chunkAckResends,
                                          chunkDeviceSnr, chunkDeviceRssi, chunkProactiveAckResends);
  postOtaStats(ann, manifest, chunkRetries, chunkSnr, chunkAckResends, chunkDeviceSnr, chunkDeviceRssi, chunkProactiveAckResends, startSeq, transferOk);
  ota_progress_end(transferOk);
  if(!transferOk) {
    ota_status("OTA: firmware transfer failed");
    sendAbort(ann, OTA_ERR_TIMEOUT);
    applyRadioMode(false);
    s_busy = false;
    return;
  }

  ota_status(String("OTA: transfer duration ") + String((millis() - transferStartMs) / 1000.0f, 2) + " s");
  //ota_status("OTA: transfer done, sending finish");
  if(!sendFinish(ann, manifest)) {
    sendAbort(ann, OTA_ERR_TIMEOUT);
    applyRadioMode(false);
    s_busy = false;
    return;
  }

  OtaAckInfo finishAck;
  if(waitForAck(ann.nonce, finishAck, 5000) && finishAck.status == OTA_STATE_READY) {
    ota_status("OTA: finished successfully");
  } else if(finishAck.status != 0) {
    ota_status(String("OTA: finish rejected by device, status=0x") + String(finishAck.status, HEX));
  } else {
    ota_status("OTA: finish not acknowledged as ready");
  }

  applyRadioMode(false);
  s_busy = false;
}

#else

void ota_gs_begin() {}
void ota_gs_try_update(const hwInfoData& info) { (void)info; }

#endif
