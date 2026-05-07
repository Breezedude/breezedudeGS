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

extern SX1262 radio_sx1262;
extern PhysicalLayer* radio_phy;
extern volatile bool receivedFlag;
extern void webconsole_print(String in);

#ifndef OTA_GS_BACKEND_URL
  #define OTA_GS_BACKEND_URL "http://fanet.breezedude.de/api/ota/check"
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
static constexpr uint8_t OTA_PROTO = 3u;
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
  radio_phy->standby();
  radio_phy->setFrequency(OTA_GS_FAST_FREQ_MHZ);
  radio_sx1262.setBandwidth((float)(fastMode ? OTA_GS_FAST_BW_KHZ : 250));
  radio_sx1262.setSpreadingFactor(fastMode ? OTA_GS_FAST_SF : 7);
  radio_sx1262.setCodingRate(fastMode ? OTA_GS_FAST_CR : 5);
  radio_sx1262.setSyncWord(fastMode ? OTA_GS_FAST_SYNCWORD : 0xF1);
  radio_sx1262.setPreambleLength(fastMode ? OTA_GS_FAST_PREAMBLE : 12);
  receivedFlag = false;
  radio_phy->startReceive();
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

static bool parseOtaAck(const uint8_t* packet, size_t len, uint32_t nonce, uint16_t& nextSeq, uint8_t& status) {
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

  nextSeq = ack->next_seq;
  status = ack->status;
  return true;
}

static bool waitForAck(uint32_t nonce, uint16_t& nextSeq, uint8_t& status, uint32_t timeoutMs) {
  uint8_t packet[255] = {0};
  size_t len = 0;
  while(waitForPacket(packet, len, timeoutMs)) {
    if(parseOtaAck(packet, len, nonce, nextSeq, status)) {
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

static bool streamFirmware(const OtaAnnouncement& ann, const OtaManifest& manifest, const uint8_t* firmwareImage) {
  if(!firmwareImage) {
    ota_status("OTA: no buffered firmware image");
    return false;
  }

  //ota_status(String("OTA: transfer started, ") + manifest.imageSize + " bytes");

  const uint32_t transferStartMs = millis();
  uint8_t chunk[OTA_MAX_CHUNK] = {0};
  uint16_t seq = 0;
  uint32_t sent = 0;
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
    for(uint8_t attempt = 0; attempt < 6 && !accepted; ++attempt) {
      if(attempt > 0u) {
        delay((uint32_t)OTA_GS_RETRY_BACKOFF_MS * attempt);
      }

      if(!transmitPacket((const uint8_t*)&pkt, sizeof(ota_pkt_prefix_t) + 4u + got)) {
        continue;
      }

      uint16_t nextSeq = 0;
      uint8_t status = 0;
      if(waitForAck(ann.nonce, nextSeq, status, OTA_GS_ACK_TIMEOUT_MS)) {
        if(status == OTA_STATE_RX && nextSeq >= (uint16_t)(seq + 1u)) {
          accepted = true;
          delay(OTA_GS_POST_ACK_DELAY_MS);
        } else if(status == OTA_ERR_SIZE) {
          ota_status(String("OTA: device rejected size at seq ") + seq);
          return false;
        } else {
          ota_status(String("OTA: ack mismatch seq ") + seq + " status=0x" + String(status, HEX) + " next=" + nextSeq);
        }
      } else if(attempt == 0u || attempt >= 4u) {
        ota_status(String("OTA: ack timeout seq ") + seq + " try=" + (attempt + 1u) + " t=" + String((millis() - transferStartMs) / 1000.0f, 2) + "s");
      }
    }

    if(!accepted) {
      ota_status(String("OTA: chunk retransmit failed at seq ") + seq + " after " + String((millis() - transferStartMs) / 1000.0f, 2) + " s");
      return false;
    }

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

  uint16_t nextSeq = 0;
  uint8_t status = 0;
  //ota_status("OTA: waiting for device handshake response");
  if(!waitForAck(ann.nonce, nextSeq, status, 4000) || status != OTA_STATE_RX) {
    ota_status("OTA: start handshake not accepted by device");
    sendAbort(ann, OTA_ERR_TIMEOUT);
    applyRadioMode(false);
    s_busy = false;
    return;
  }

  ota_status("OTA: handshake successful, transferring firmware");
  const uint32_t transferStartMs = millis();
  if(!streamFirmware(ann, manifest, firmwareImage.get())) {
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

  if(waitForAck(ann.nonce, nextSeq, status, 5000) && status == OTA_STATE_READY) {
    ota_status("OTA: finished successfully");
  } else if(status != 0) {
    ota_status(String("OTA: finish rejected by device, status=0x") + String(status, HEX));
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
