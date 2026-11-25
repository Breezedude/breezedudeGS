#include "bd_fwstore.h"
#include "bd_json_out.h"
#include "ota_gs.h"
#include "config_gs.h"

#if BREEZEDUDE_GS_ENABLE_LORA_OTA

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <RadioLib.h>
#include <algorithm>
#include <memory>
#include <new>
#include <vector>
#include <math.h>

#include "helper.h"
#include "bd_net.h"
#include "bd_power.h"
#include "bd_state.h"
#include "bd_web.h"
#include "freertos/semphr.h"

// ESP-IDF port: receivedFlag is no longer a plain bool. The DIO1 line doubles
// as the light-sleep wake source, which only works level triggered, so the flag
// object re-arms the masked interrupt when it is cleared. Declared in helper.h.
extern void webconsole_print(String in);
extern void ota_status_v(const String& msg);
extern void ota_progress_start(uint32_t totalChunks);
extern void ota_progress_chunk(uint16_t seq, uint8_t retries, float snr, uint8_t ackResendCount,
                                int8_t deviceSnr, int8_t deviceRssi, uint8_t proactiveAckResendCount);
extern void ota_progress_end(bool success);
extern Settings settings;
extern String fw_version;

#ifndef OTA_GS_BACKEND_URL
  #define OTA_GS_BACKEND_URL "http://fanet.breezedude.de/api/ota/check"
#endif
#ifndef OTA_GS_STATS_URL
  #define OTA_GS_STATS_URL "http://fanet.breezedude.de/api/ota/stats"
#endif
#ifndef OTA_GS_PRELOAD_URL
  #define OTA_GS_PRELOAD_URL "http://fanet.breezedude.de/api/ota/preload"
#endif
#ifndef OTA_GS_RESULT_URL
  #define OTA_GS_RESULT_URL "http://fanet.breezedude.de/api/ota/result"
#endif
// The result report is sent after the radio is back in FANET mode, but it still
// blocks the caller - keep it short, it is best effort and never retried.
#ifndef OTA_GS_RESULT_TIMEOUT_MS
  #define OTA_GS_RESULT_TIMEOUT_MS 3000
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
static constexpr uint8_t OTA_PROTO = 4u;      // preferred / newest wire protocol
static constexpr uint8_t OTA_PROTO_MIN = 3u;  // oldest wire protocol we can still speak
// v3 and v4 share identical offer/chunk/finish/abort layouts and signed-manifest
// payload; they differ only in the proto byte and the ACK trailer (v4 appends
// ack_resend_count/chunk_snr/chunk_rssi/proactive_ack_resend_count). The GS
// therefore speaks whichever version the device advertises in its HW-Info
// announce (ann.otaProto), selected per transfer via s_active_proto below.
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
// START handshake: resent this often until the device answers, for at most
// this long in total (see the handshake in ota_gs_try_update).
static constexpr uint32_t OTA_GS_START_RETRY_MS = 1500UL;
static constexpr uint32_t OTA_GS_START_TOTAL_MS = 12000UL;
static constexpr uint16_t OTA_GS_POST_ACK_DELAY_MS = 35u;
static constexpr uint16_t OTA_GS_RETRY_BACKOFF_MS = 80u;
static constexpr uint8_t OTA_GS_CHUNK_MAX_ATTEMPTS = 15u;
// FINISH: sent this often at most, each time waiting this long for the answer.
static constexpr uint8_t OTA_GS_FINISH_SENDS = 3u;
static constexpr uint32_t OTA_GS_FINISH_WAIT_MS = 5000UL;
// Resume: set in the START packet's chunk_size field (not part of the signed
// data) to tell the sensor that this station continues a transfer at the
// sequence number the sensor answers with. A sensor that was interrupted while
// receiving the same image then keeps what it has instead of erasing its slot.
// Sensor firmware from 1.2.3 on knows the flag; older ones are not sent it.
static constexpr uint16_t OTA_START_FLAG_RESUME = 0x8000u;
static constexpr uint16_t OTA_RESUME_MIN_VERSION_BCD = 0x123u;
// Hold: sensor firmware from 1.2.3 on listens for 300 ms after every HW info
// packet that is not an OTA announcement. A FANET ACK in that window makes it
// send the announcement at once, so an update does not have to wait for the
// sensor's hourly one. Sent without asking the backend - there is no time for
// that - and only when the image is already in the flash store and the backend
// named this station as the one to do it (see ota_gs_hold()).
static constexpr uint16_t OTA_HOLD_MIN_VERSION_BCD = 0x123u;
// Not again for the same sensor within this time: repeated copies of a packet
// come in seconds later, when the sensor no longer listens.
static constexpr uint32_t OTA_HOLD_REPEAT_MS = 60000UL;

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
  float snr = 0;  // and its SNR
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
  bool hold = false;  // /preload only: this station may ask the sensor for its announcement
};

// Timing and progress of one OTA attempt, collected while it runs and sent to
// the backend by reportOtaResult() where the attempt ends.
struct OtaAttempt {
  uint32_t announceMs = 0;  // millis() when the announcement was handled
  uint32_t checkMs = 0;     // duration of the /check request
  uint32_t downloadMs = 0;  // duration of the firmware download, 0 if not reached
  uint32_t chunksSent = 0;  // chunks accepted by the device
  uint32_t totalChunks = 0;
  uint8_t startSends = 0;   // START packets sent before the device answered
  uint32_t startAckMs = 0;  // from the first START to the device's answer, 0 without one
};

// Last few update decisions per sensor, for the station details in the web UI.
// Written by the radio task, read by the web server, hence the lock.
static constexpr uint8_t OTA_HISTORY_DEVICES = 16u;
static constexpr uint8_t OTA_HISTORY_ENTRIES = 5u;

struct OtaHistoryEntry {
  uint32_t ts;
  uint16_t fromBcd;
  uint16_t toBcd;
  char outcome[20];
  char detail[32];
};

// The sensor sends its OTA announcement (HW info debug type 2) once an hour,
// the first 10 minutes after boot, and only listens for an offer right after
// it. The other HW info types come every 10 to 60 minutes.
static constexpr uint32_t OTA_ANNOUNCE_PERIOD_S = 3600UL;
static constexpr uint32_t OTA_FIRST_ANNOUNCE_S = 600UL;
// A predicted window counts as missed this long after its time.
static constexpr uint32_t OTA_WINDOW_GRACE_S = 300UL;
// The uptime field wraps after 65536 minutes.
static constexpr uint32_t OTA_UPTIME_WRAP_S = 65536UL * 60UL;
// Ask the backend about a sensor's next firmware at most this often.
static constexpr uint32_t OTA_PRELOAD_RECHECK_MS = 3600000UL;
// Same limits as the backend's offer (routes_ota.js): no point in fetching an
// image for a sensor this station hears too weakly to update.
static constexpr int OTA_PRELOAD_MIN_RSSI = -115;
static constexpr float OTA_PRELOAD_MIN_SNR = -8.0f;

enum OtaPreloadState : uint8_t {
  PRELOAD_NONE = 0,     // not asked yet
  PRELOAD_NOT_DUE,      // backend: no update for this sensor
  PRELOAD_READY,        // image in the flash store
  PRELOAD_FAILED,       // backend unreachable or download failed
};

struct OtaHistory {
  bool used;
  uint8_t vendor;
  uint16_t address;
  uint8_t count;
  uint8_t head;  // next entry to write
  uint32_t lastUse;
  OtaHistoryEntry entries[OTA_HISTORY_ENTRIES];

  // From the HW info packets, see ota_gs_note_hwinfo(). Times are Unix seconds.
  uint16_t versionBcd;       // from the last announcement, 0 = none heard
  uint32_t bootEpoch;        // now - uptime, 0 = unknown
  uint32_t lastAnnounce;     // last announcement heard here
  uint32_t nextWindow;       // predicted next announcement, 0 = unknown
  uint8_t missedWindows;     // predicted windows without an announcement, in a row
  uint32_t typeSeen[5];      // last packet per debug type 1-4 (index 0 unused)
  int16_t rssi;
  int16_t snr10;

  uint8_t preloadState;
  bool preloadWanted;
  uint16_t preloadTargetBcd;
  uint32_t preloadCheckedMs;  // millis() of the last backend answer, 0 = never
  uint32_t preloadAt;         // Unix time of that answer
  char preloadDetail[32];
  bool holdAllowed;           // backend: this station asks the sensor, see ota_gs_hold()
  uint32_t lastHoldMs;        // millis() of the last hold sent, 0 = none
};

static OtaHistory s_history[OTA_HISTORY_DEVICES];
static portMUX_TYPE s_history_lock = portMUX_INITIALIZER_UNLOCKED;

static String historyDeviceJson(const OtaHistory& h);

static bool s_busy = false;
static uint32_t s_last_nonce = 0;
static uint32_t s_last_attempt_ms = 0;
// Wire protocol used for the device currently being served. Set from
// ann.otaProto at the start of ota_gs_try_update(); drives both the outgoing
// prefix.proto and how the device's ACK is parsed.
static uint8_t s_active_proto = OTA_PROTO;

// Common ACK length shared by all protocols (through accepted_chunk). v4 appends
// a 4-byte telemetry trailer; v3 devices send only this common prefix.
static constexpr size_t OTA_ACK_LEN_COMMON = sizeof(ota_ack_pkt_t) - 4u;

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
  ann.snr = info.snr;
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

// On failure `error` holds a short description if the backend was reached and
// answered; it stays empty when the request never got there (no report then).
static bool queryBackend(const OtaAnnouncement& ann, OtaManifest& manifest, String& error) {
  const String url = String(OTA_GS_BACKEND_URL) +
                     "?vendor=" + String(ann.vendor) +
                     "&address=" + String(ann.address) +
                     "&nonce=" + String(ann.nonce) +
                     "&versionBcd=" + String(ann.versionBcd) +
                     "&slotCapacityKb=" + String(ann.slotCapacityKb) +
                     "&rssi=" + String(ann.rssi) +
                     "&snr=" + String(ann.snr, 1);

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
  http.addHeader("ID", settings.deviceName);
  code = http.GET();

  if(code != HTTP_CODE_OK) {
    ota_status(String("OTA: backend HTTP error ") + code);
    if(code > 0) {
      error = String("HTTP ") + code;
    }
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
    error = String("JSON parse failed: ") + err.c_str();
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
    error = "manifest incomplete";
    manifest.available = false;
    return false;
  }

  const String signatureRawHex(signatureRawHexCstr);
  if(!parseHexBytes(manifest.imageSha256, manifest.imageSha256Raw, sizeof(manifest.imageSha256Raw)) ||
     !parseHexBytes(signatureRawHex, manifest.signatureRaw, sizeof(manifest.signatureRaw))) {
    ota_status("OTA: signed manifest payload invalid");
    error = "signed manifest payload invalid";
    manifest.available = false;
    return false;
  }

  const uint32_t nowTs = (uint32_t)time(nullptr);
  if(manifest.expires && nowTs && nowTs > manifest.expires) {
    ota_status("OTA: manifest expired");
    error = String("manifest expired (") + manifest.expires + " < " + nowTs + ")";
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
  prefix.proto = s_active_proto;
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
  // Accept the common (v3) length; the v4 telemetry trailer is optional.
  if(len < OTA_ACK_LEN_COMMON) {
    return false;
  }

  const ota_ack_pkt_t* ack = (const ota_ack_pkt_t*)packet;
  if(ack->prefix.magic0 != OTA_MAGIC0 || ack->prefix.magic1 != OTA_MAGIC1 ||
     ack->prefix.proto != s_active_proto || ack->prefix.op != OTA_OP_ACK) {
    return false;
  }
  if(ack->nonce != nonce) {
    return false;
  }

  info.nextSeq = ack->next_seq;
  info.status = ack->status;

  // v4 devices append per-chunk telemetry; v3 devices do not. Only read the
  // trailer when the device speaks v4 and actually sent the full-length ACK.
  if(s_active_proto >= 4u && len >= sizeof(ota_ack_pkt_t)) {
    info.ackResendCount = ack->ack_resend_count;
    info.chunkSnr = ack->chunk_snr;
    info.chunkRssi = ack->chunk_rssi;
    info.proactiveAckResendCount = ack->proactive_ack_resend_count;
  } else {
    info.ackResendCount = 0;
    info.chunkSnr = 0;
    info.chunkRssi = 0;
    info.proactiveAckResendCount = 0;
  }
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
  if(ann.versionBcd >= OTA_RESUME_MIN_VERSION_BCD) {
    pkt.chunk_size |= OTA_START_FLAG_RESUME;
  }
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

// The image comes from the flash store (storage/bd_fwstore.cpp) when it is
// there, else it is downloaded into it. During the radio transfer it is read
// from flash, so neither RAM nor WiFi matter then. `cached` tells which.
// On failure `error` holds a short reason for the result report.
// One writer at a time on the flash store: the preload in the net task or the
// update in the radio task.
static SemaphoreHandle_t s_store_mutex = nullptr;

static bool storeTryLock(TickType_t wait) {
  if(!s_store_mutex) {
    return false;
  }
  return xSemaphoreTake(s_store_mutex, wait) == pdTRUE;
}

static void storeUnlock() { xSemaphoreGive(s_store_mutex); }

static bool downloadFirmwareImage(const OtaManifest& manifest, BdFwImage& image, String& error);

static bool obtainFirmwareImage(const OtaManifest& manifest, BdFwImage& image, bool& cached, String& error) {
  cached = bd_fwstore_find(manifest.imageSha256Raw, manifest.imageSize, &image);
  if(cached) {
    ota_status(String("OTA: firmware ") + manifest.fileName + " from the flash store");
    return true;
  }
  // WiFi power save cuts the download to ~12 KB/s: 10-30 s for an image, while
  // the sensor waits 5-15 s for the answer to its announcement.
  bd_power_hold_awake(true);
  bd_net_bulk_transfer(true);
  const bool ok = downloadFirmwareImage(manifest, image, error);
  bd_net_bulk_transfer(false);
  bd_power_hold_awake(false);
  return ok;
}

static bool downloadFirmwareImage(const OtaManifest& manifest, BdFwImage& image, String& error) {
  // Before the download, which would be for nothing.
  BdFwWriter writer;
  if(!bd_fwstore_begin(manifest.imageSize, &writer)) {
    ota_status(String("OTA: no room in the flash store for ") + manifest.imageSize + " bytes");
    error = "no store";
    return false;
  }
  uint8_t* buf = (uint8_t*)malloc(2048);
  if(!buf) {
    error = "no RAM";
    return false;
  }

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
    error = "begin failed";
    free(buf);
    return false;
  }

  http.addHeader("ID", settings.deviceName);
  code = http.GET();
  if(code != HTTP_CODE_OK) {
    ota_status(String("OTA: firmware download HTTP error ") + code);
    error = String("HTTP ") + code;
    http.end();
    free(buf);
    return false;
  }

  ota_status(String("OTA: preloading firmware, ") + manifest.imageSize + " bytes");
  WiFiClient* stream = http.getStreamPtr();
  size_t received = 0;
  uint8_t lastProgress = 0;
  uint32_t stallDeadline = millis() + 15000UL;

  while(received < manifest.imageSize) {
    if(stream->available()) {
      const size_t want = min((size_t)2048u, (size_t)(manifest.imageSize - received));
      const int got = stream->read(buf, (int)want);
      if(got > 0) {
        if(!bd_fwstore_write(&writer, buf, (size_t)got)) {
          break;
        }
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
  free(buf);
  if(received != manifest.imageSize) {
    ota_status(String("OTA: firmware preload incomplete ") + received + "/" + manifest.imageSize + " bytes");
    error = String(received) + "/" + manifest.imageSize + " bytes";
    return false;
  }
  if(!bd_fwstore_finish(&writer, manifest.imageSha256Raw, manifest.fileName.c_str(), &image)) {
    ota_status("OTA: downloaded firmware does not match its SHA-256");
    error = "checksum";
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
static bool streamFirmware(const OtaAnnouncement& ann, const OtaManifest& manifest, const BdFwImage& firmwareImage,
                            uint16_t startSeq, std::vector<uint8_t>& chunkRetries, std::vector<float>& chunkSnr,
                            std::vector<uint8_t>& chunkAckResends, std::vector<int8_t>& chunkDeviceSnr,
                            std::vector<int8_t>& chunkDeviceRssi, std::vector<uint8_t>& chunkProactiveAckResends) {
  if(!firmwareImage.part) {
    ota_status("OTA: no stored firmware image");
    return false;
  }

  //ota_status(String("OTA: transfer started, ") + manifest.imageSize + " bytes");

  const uint32_t transferStartMs = millis();
  uint8_t chunk[OTA_MAX_CHUNK] = {0};
  uint16_t seq = startSeq;
  // A sensor that already holds every chunk (only FINISH was lost) answers
  // with the chunk count; the last chunk may be a short one.
  uint32_t sent = std::min<uint32_t>((uint32_t)startSeq * manifest.chunkSize, manifest.imageSize);
  uint8_t lastProgress = 0;

  while(sent < manifest.imageSize) {
    const size_t want = min((size_t)manifest.chunkSize, (size_t)(manifest.imageSize - sent));
    if(!bd_fwstore_read(firmwareImage, sent, chunk, want)) {
      ota_status(String("OTA: reading the stored image failed at ") + sent);
      return false;
    }
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
  http.addHeader("ID", settings.deviceName);
  const int code = http.POST(payload.data(), payload.size());
  if(code != HTTP_CODE_OK && code != HTTP_CODE_CREATED && code != HTTP_CODE_NO_CONTENT) {
    ota_status(String("OTA: stats upload HTTP error ") + code);
  }
  http.end();
}

static const char* preloadStateText(uint8_t state) {
  switch(state) {
    case PRELOAD_NOT_DUE: return "not_due";
    case PRELOAD_READY: return "ready";
    case PRELOAD_FAILED: return "failed";
    default: return "none";
  }
}

static String historyDeviceJson(const OtaHistory& h) {
  JsonDocument doc;
  char id[8];
  snprintf(id, sizeof(id), "%02X%04X", h.vendor, h.address);
  doc["id"] = id;

  // When the sensor will listen for an update next, as far as its packets tell.
  JsonObject window = doc["window"].to<JsonObject>();
  if(h.versionBcd) window["version"] = formatVersionBcd(h.versionBcd);
  if(h.lastAnnounce) window["last"] = h.lastAnnounce;
  if(h.nextWindow) window["next"] = h.nextWindow;
  window["from_boot"] = h.nextWindow && !h.lastAnnounce;
  window["missed"] = h.missedWindows;
  JsonObject types = window["types"].to<JsonObject>();
  for(uint8_t t = 1; t <= 4; ++t) {
    if(h.typeSeen[t]) types[String(t)] = h.typeSeen[t];
  }
  JsonObject preload = doc["preload"].to<JsonObject>();
  preload["state"] = preloadStateText(h.preloadState);
  if(h.preloadTargetBcd) preload["target"] = formatVersionBcd(h.preloadTargetBcd);
  if(h.preloadAt) preload["at"] = h.preloadAt;
  if(h.preloadDetail[0]) preload["detail"] = h.preloadDetail;

  JsonArray entries = doc["entries"].to<JsonArray>();
  for(uint8_t i = 0; i < h.count; ++i) {
    const OtaHistoryEntry& e = h.entries[(h.head + OTA_HISTORY_ENTRIES - h.count + i) % OTA_HISTORY_ENTRIES];
    JsonObject o = entries.add<JsonObject>();
    o["ts"] = e.ts;
    o["from"] = formatVersionBcd(e.fromBcd);
    if(e.toBcd) {
      o["to"] = formatVersionBcd(e.toBcd);
    }
    o["outcome"] = e.outcome;
    if(e.detail[0]) {
      o["detail"] = e.detail;
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// The sensor's slot, taken over from the one unused longest when it has none.
// Call with s_history_lock held.
static OtaHistory* historySlotLocked(uint8_t vendor, uint16_t address) {
  OtaHistory* oldest = &s_history[0];
  for(OtaHistory& h : s_history) {
    if(h.used && h.vendor == vendor && h.address == address) {
      return &h;
    }
    if(!h.used || (oldest->used && (int32_t)(h.lastUse - oldest->lastUse) < 0)) {
      oldest = &h;
    }
  }
  memset(oldest, 0, sizeof(*oldest));
  oldest->used = true;
  oldest->vendor = vendor;
  oldest->address = address;
  oldest->lastUse = millis();
  return oldest;
}

static void broadcastHistory(const OtaHistory& snapshot) {
  bd_ws_broadcast("{\"ota_history\":[" + historyDeviceJson(snapshot) + "]}");
}

// Records one update decision for the station details and pushes the sensor's
// updated history to open web UIs.
static void recordOtaHistory(const OtaAnnouncement& ann, uint16_t toBcd, const char* outcome,
                             const String& detail = String()) {
  OtaHistory snapshot;
  portENTER_CRITICAL(&s_history_lock);
  OtaHistory* slot = historySlotLocked(ann.vendor, ann.address);
  OtaHistoryEntry& e = slot->entries[slot->head];
  e.ts = (uint32_t)time(nullptr);
  e.fromBcd = ann.versionBcd;
  e.toBcd = toBcd;
  strlcpy(e.outcome, outcome, sizeof(e.outcome));
  strlcpy(e.detail, detail.c_str(), sizeof(e.detail));
  slot->head = (uint8_t)((slot->head + 1u) % OTA_HISTORY_ENTRIES);
  if(slot->count < OTA_HISTORY_ENTRIES) {
    slot->count++;
  }
  slot->lastUse = millis();
  snapshot = *slot;
  portEXIT_CRITICAL(&s_history_lock);

  broadcastHistory(snapshot);
}

bool ota_gs_busy() { return s_busy; }

void ota_gs_history_write(BdJsonOut& out) {
  bool first = true;
  for(uint8_t i = 0; i < OTA_HISTORY_DEVICES && out.ok(); ++i) {
    OtaHistory h;
    portENTER_CRITICAL(&s_history_lock);
    h = s_history[i];
    portEXIT_CRITICAL(&s_history_lock);
    if(!h.used) {
      continue;
    }
    if(!first) {
      out.text(",");
    }
    first = false;
    out.text(historyDeviceJson(h).c_str());
  }
}

// Tells the backend where an OTA attempt ended (reason "success" = installed).
// Called once per attempt the backend offered an update for, after the radio is
// back in FANET mode. One short POST, no retry; an endpoint the server does not
// know yet (404) is tolerated silently.
static void reportOtaResult(const OtaAnnouncement& ann, const OtaManifest& manifest, const OtaAttempt& attempt,
                            const char* stage, const char* reason, const String& detail = String()) {
  recordOtaHistory(ann, manifest.targetVersionBcd, reason, detail);
  if(WiFi.status() != WL_CONNECTED) {
    return;
  }

  JsonDocument doc;
  doc["vendor"] = ann.vendor;
  doc["address"] = ann.address;
  doc["nonce"] = ann.nonce;
  doc["version_bcd"] = ann.versionBcd;
  doc["target_version_bcd"] = manifest.targetVersionBcd;
  doc["ota_proto"] = ann.otaProto;
  doc["stage"] = stage;
  doc["result"] = strcmp(reason, "success") == 0 ? "ok" : "failed";
  doc["reason"] = reason;
  if(detail.length()) {
    doc["detail"] = detail;
  }
  doc["ms_check"] = attempt.checkMs;
  doc["ms_download"] = attempt.downloadMs;
  doc["ms_since_announce"] = millis() - attempt.announceMs;
  doc["chunks_sent"] = attempt.chunksSent;
  doc["total_chunks"] = attempt.totalChunks;
  doc["gs_fw"] = fw_version;
  // Link and handshake details, for the update statistics: how well this
  // station heard the announcement, how many START packets it took, and
  // whether FLARM/ADS-L share the radio.
  doc["ann_rssi"] = ann.rssi;
  doc["ann_snr"] = roundf(ann.snr * 10.0f) / 10.0f;
  doc["start_sends"] = attempt.startSends;
  doc["start_ack_ms"] = attempt.startAckMs;
  doc["fsk"] = flarm_enabled() || adsl_enabled();
  String body;
  serializeJson(doc, body);
  Serial.println("OTA: result report " + body);

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(OTA_GS_RESULT_TIMEOUT_MS);
  http.setTimeout(OTA_GS_RESULT_TIMEOUT_MS);
  if(!http.begin(client, OTA_GS_RESULT_URL)) {
    ota_status("OTA: result report begin failed");
    return;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("ID", settings.deviceName);
  const int code = http.POST(body);
  if(code <= 0) {
    ota_status(String("OTA: result report failed ") + code);
  }
  http.end();
}

void ota_gs_begin() {
  s_busy = false;
  if(!s_store_mutex) {
    s_store_mutex = xSemaphoreCreateMutex();
  }
}

// Next announcement after `now`, from the last one heard or else from the boot
// time. 0 when neither is known.
static uint32_t predictWindow(const OtaHistory& h, uint32_t now) {
  uint32_t base;
  if(h.lastAnnounce) {
    base = h.lastAnnounce + OTA_ANNOUNCE_PERIOD_S;
  } else if(h.bootEpoch) {
    base = h.bootEpoch + OTA_FIRST_ANNOUNCE_S;
  } else {
    return 0;
  }
  if(base + OTA_WINDOW_GRACE_S >= now) {
    return base;
  }
  const uint32_t periods = (now - OTA_WINDOW_GRACE_S - base) / OTA_ANNOUNCE_PERIOD_S + 1u;
  return base + periods * OTA_ANNOUNCE_PERIOD_S;
}

// A boot time within two minutes of the stored one, or off by whole uptime
// wraps, is the same boot (the uptime has minute resolution).
static bool sameBoot(uint32_t a, uint32_t b) {
  const uint32_t d = a > b ? a - b : b - a;
  const uint32_t r = d % OTA_UPTIME_WRAP_S;
  return r <= 120u || OTA_UPTIME_WRAP_S - r <= 120u;
}

void ota_gs_note_hwinfo(const hwInfoData& info) {
  if(info.vid == 0 || !info.hasSubtypeBuildDate || info.rawLen == 0) {
    return;
  }
  const uint32_t now = (uint32_t)time(nullptr);
  if(now < 1700000000UL) {
    return;  // no clock yet, nothing to predict with
  }
  const uint8_t type = info.rawAfterBuildDate[0];

  OtaHistory snapshot;
  portENTER_CRITICAL(&s_history_lock);
  OtaHistory* h = historySlotLocked(info.vid, info.fanet_id);
  h->lastUse = millis();
  if(type >= 1 && type <= 4) {
    h->typeSeen[type] = now;
  }
  h->rssi = (int16_t)info.rssi;
  h->snr10 = (int16_t)lroundf(info.snr * 10.0f);

  if(info.hasUptime) {
    const uint32_t boot = now - (uint32_t)info.uptimeMinutes * 60u;
    if(h->bootEpoch && !sameBoot(boot, h->bootEpoch)) {
      // Restarted: the hourly rhythm starts over 10 minutes after boot.
      h->lastAnnounce = 0;
      h->nextWindow = 0;
      h->missedWindows = 0;
    }
    if(!h->bootEpoch || !sameBoot(boot, h->bootEpoch)) {
      h->bootEpoch = boot;
    }
  }

  if(type == OTA_DEBUG_TYPE && info.rawLen >= 3u) {
    const uint16_t version = readLe16(&info.rawAfterBuildDate[1]);
    if(version != h->versionBcd) {
      h->preloadCheckedMs = 0;  // a new version may need a different image
    }
    h->versionBcd = version;
    h->lastAnnounce = now;
    h->missedWindows = 0;
  } else if(h->nextWindow && now > h->nextWindow + OTA_WINDOW_GRACE_S) {
    const uint32_t passed = (now - OTA_WINDOW_GRACE_S - h->nextWindow) / OTA_ANNOUNCE_PERIOD_S + 1u;
    h->missedWindows = (uint8_t)std::min<uint32_t>(255u, h->missedWindows + passed);
  }
  h->nextWindow = predictWindow(*h, now);

  // Fetch the image the backend will offer before the sensor asks for it.
  const bool linkOk = info.rssi >= OTA_PRELOAD_MIN_RSSI && info.snr >= OTA_PRELOAD_MIN_SNR;
  if(linkOk && (h->preloadCheckedMs == 0 || millis() - h->preloadCheckedMs >= OTA_PRELOAD_RECHECK_MS)) {
    h->preloadWanted = true;
  }
  const bool wanted = h->preloadWanted;
  snapshot = *h;
  portEXIT_CRITICAL(&s_history_lock);

  broadcastHistory(snapshot);
  if(wanted) {
    bd_net_reevaluate();
  }
}

// GET <preload>?vendor&address[&versionBcd]: the image the next /check would
// offer this sensor, without signature or arbitration. False if the backend
// was not reached or did not answer usefully; `error` says why.
static bool queryPreload(uint8_t vendor, uint16_t address, uint16_t versionBcd, OtaManifest& manifest,
                         String& error) {
  String url = String(OTA_GS_PRELOAD_URL) + "?vendor=" + String(vendor) + "&address=" + String(address);
  if(versionBcd) {
    url += "&versionBcd=" + String(versionBcd);
  }
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  if(!http.begin(client, url)) {
    error = "begin failed";
    return false;
  }
  http.addHeader("ID", settings.deviceName);
  const int code = http.GET();
  if(code != HTTP_CODE_OK) {
    error = code > 0 ? String("HTTP ") + code : String("no answer");
    http.end();
    return false;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if(err) {
    error = "bad JSON";
    return false;
  }
  manifest.available = doc["available"] | false;
  manifest.noUpdateReason = (const char*)(doc["reason"] | "");
  if(!manifest.available) {
    return true;
  }
  manifest.firmwareUrl = (const char*)(doc["firmwareUrl"] | "");
  manifest.fileName = (const char*)(doc["fileName"] | "");
  manifest.targetVersionBcd = doc["targetVersionBcd"] | (uint16_t)0u;
  manifest.imageSize = doc["imageSize"] | 0u;
  manifest.imageSha256 = (const char*)(doc["imageSha256"] | "");
  manifest.hold = doc["hold"] | false;
  if(manifest.firmwareUrl.isEmpty() || manifest.imageSize == 0u ||
     !parseHexBytes(manifest.imageSha256, manifest.imageSha256Raw, sizeof(manifest.imageSha256Raw))) {
    error = "manifest incomplete";
    manifest.available = false;
    return false;
  }
  return true;
}

// Called for every HW info packet as soon as it is decoded, before anything
// that takes time: the sensor listens for only 300 ms. Sends the FANET ACK
// that makes it announce itself, when an update for it is ready here. The
// announcement that follows runs through ota_gs_try_update() like any other.
bool ota_gs_hold(const hwInfoData& info) {
  if(s_busy || otaUploadInProgress || WiFi.status() != WL_CONNECTED || info.vid == 0 ||
     !info.hasSubtypeBuildDate || info.rawLen == 0) {
    return false;
  }
  const uint8_t type = info.rawAfterBuildDate[0];
  if(type < 1 || type > 4 || type == OTA_DEBUG_TYPE) {
    return false;  // after its announcement the sensor waits for the offer, not for this
  }

  bool send = false;
  portENTER_CRITICAL(&s_history_lock);
  for(OtaHistory& h : s_history) {
    if(!h.used || h.vendor != info.vid || h.address != info.fanet_id) {
      continue;
    }
    send = h.preloadState == PRELOAD_READY && h.holdAllowed &&
           h.versionBcd >= OTA_HOLD_MIN_VERSION_BCD && h.preloadTargetBcd > h.versionBcd &&
           (h.lastHoldMs == 0 || millis() - h.lastHoldMs >= OTA_HOLD_REPEAT_MS);
    if(send) {
      h.lastHoldMs = millis() | 1u;
    }
    break;
  }
  portEXIT_CRITICAL(&s_history_lock);
  if(!send) {
    return false;
  }

  OtaAnnouncement ann;
  ann.vendor = info.vid;
  ann.address = info.fanet_id;
  const bool sent = sendFanetAck(ann);
  receivedFlag = false;
  radio_phy->startReceive();
  if(sent) {
    char deviceId[8];
    snprintf(deviceId, sizeof(deviceId), "%02X%04X", info.vid, info.fanet_id);
    ota_status(String("OTA: update ready for ") + deviceId + ", asked for its announcement");
  }
  return sent;
}

void ota_gs_preload_tick() {
  if(!s_store_mutex || s_busy || otaUploadInProgress || WiFi.status() != WL_CONNECTED) {
    return;
  }

  // One sensor per call; the net task comes back within seconds.
  uint8_t vendor = 0;
  uint16_t address = 0;
  uint16_t versionBcd = 0;
  bool found = false;
  portENTER_CRITICAL(&s_history_lock);
  for(OtaHistory& h : s_history) {
    if(h.used && h.preloadWanted) {
      h.preloadWanted = false;
      vendor = h.vendor;
      address = h.address;
      versionBcd = h.versionBcd;
      found = true;
      break;
    }
  }
  portEXIT_CRITICAL(&s_history_lock);
  if(!found) {
    return;
  }

  char deviceId[8];
  snprintf(deviceId, sizeof(deviceId), "%02X%04X", vendor, address);
  OtaManifest manifest;
  String error;
  uint8_t state = PRELOAD_FAILED;
  String detail;
  if(!queryPreload(vendor, address, versionBcd, manifest, error)) {
    detail = error;
  } else if(!manifest.available) {
    state = PRELOAD_NOT_DUE;
    detail = manifest.noUpdateReason;
  } else if(!storeTryLock(0)) {
    // The radio task is updating a sensor; try again with the next packet.
    portENTER_CRITICAL(&s_history_lock);
    historySlotLocked(vendor, address)->preloadWanted = true;
    portEXIT_CRITICAL(&s_history_lock);
    return;
  } else {
    BdFwImage image;
    bool cached = false;
    if(obtainFirmwareImage(manifest, image, cached, error)) {
      state = PRELOAD_READY;
      if(!cached) {
        webconsole_print(String("OTA: firmware ") + manifest.fileName + " preloaded for " + deviceId);
      }
    } else {
      detail = error;
    }
    storeUnlock();
  }

  OtaHistory snapshot;
  portENTER_CRITICAL(&s_history_lock);
  OtaHistory* h = historySlotLocked(vendor, address);
  h->preloadState = state;
  h->holdAllowed = state == PRELOAD_READY && manifest.hold;
  h->preloadTargetBcd = manifest.available ? manifest.targetVersionBcd : 0;
  h->preloadCheckedMs = millis() | 1u;
  h->preloadAt = (uint32_t)time(nullptr);
  strlcpy(h->preloadDetail, detail.c_str(), sizeof(h->preloadDetail));
  snapshot = *h;
  portEXIT_CRITICAL(&s_history_lock);
  broadcastHistory(snapshot);
}

void ota_gs_try_update(const hwInfoData& info) {
  if(s_busy || WiFi.status() != WL_CONNECTED || info.vid == 0) {
    return;
  }

  OtaAnnouncement ann;
  if(!parseDebugType4(info, ann)) {
    return;
  }

  // Speak whatever OTA wire protocol the device advertises. v3 (firmware
  // <= 1.0.x) and v4 (firmware >= 1.1.x, and the bootloader-OTA updater) are
  // supported; anything else we cannot talk to.
  if(ann.otaProto < OTA_PROTO_MIN || ann.otaProto > OTA_PROTO) {
    ota_status(String("OTA: unsupported device OTA protocol v") + (int)ann.otaProto);
    recordOtaHistory(ann, 0, "unsupported_proto", String("v") + (int)ann.otaProto);
    return;
  }
  s_active_proto = ann.otaProto;

  if(ann.nonce == s_last_nonce && (millis() - s_last_attempt_ms) < 30000UL) {
    return;
  }
  s_last_nonce = ann.nonce;
  s_last_attempt_ms = millis();

  OtaManifest manifest;
  OtaAttempt attempt;
  attempt.announceMs = millis();
  s_busy = true;
  char deviceId[16];
  snprintf(deviceId, sizeof(deviceId), "%02X%04X", ann.vendor, ann.address);
  ota_status(String("OTA: request from ") + deviceId + " with version " + formatVersionBcd(ann.versionBcd));

  String error;
  bool ok = queryBackend(ann, manifest, error);
  attempt.checkMs = millis() - attempt.announceMs;
  if(!ok) {
    if(error.length()) {
      reportOtaResult(ann, manifest, attempt, "check", "backend_error", error);
    } else {
      recordOtaHistory(ann, 0, "backend_unreachable");
    }
    s_busy = false;
    return;
  }
  if(!manifest.available) {
    recordOtaHistory(ann, 0, manifest.suppressNoUpdateNotify ? "other_gs" : "no_update",
                     manifest.suppressNoUpdateNotify ? String() : manifest.noUpdateReason);
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

  // Pending settings first, in the same window the no-update branch uses: an
  // update that keeps failing used to hold them back for good, since this branch
  // never sent them. The station restarts to apply them, so the update waits for
  // its next report.
  if(config_gs_check_device(String(deviceId), ann.nonce)) {
    ota_status("OTA: settings applied first, update follows with the next report");
    recordOtaHistory(ann, 0, "config_first");
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

  const uint32_t totalChunks = manifest.chunkSize ? (manifest.imageSize + manifest.chunkSize - 1u) / manifest.chunkSize : 0u;
  attempt.totalChunks = totalChunks;

  if(manifest.imageSize > ((uint32_t)ann.slotCapacityKb * 1024u)) {
    ota_status("OTA: image does not fit into target flash");
    reportOtaResult(ann, manifest, attempt, "download", "image_too_large",
                    String(manifest.imageSize) + " > " + ann.slotCapacityKb + " KB");
    s_busy = false;
    return;
  }

  BdFwImage firmwareImage;
  bool cached = false;
  // A preload holding the store is fetching this or another image right now;
  // waiting for it would miss the sensor's window anyway.
  if(!storeTryLock(0)) {
    ota_status("OTA: firmware is being preloaded, the update follows with the next announcement");
    reportOtaResult(ann, manifest, attempt, "download", "preload_busy");
    s_busy = false;
    return;
  }
  const uint32_t downloadStartMs = millis();
  const bool downloaded = obtainFirmwareImage(manifest, firmwareImage, cached, error);
  storeUnlock();
  attempt.downloadMs = cached ? 0 : millis() - downloadStartMs;
  if(downloaded) {
    portENTER_CRITICAL(&s_history_lock);
    OtaHistory* slot = historySlotLocked(ann.vendor, ann.address);
    slot->preloadState = PRELOAD_READY;
    slot->preloadTargetBcd = manifest.targetVersionBcd;
    portEXIT_CRITICAL(&s_history_lock);
  }
  if(!downloaded) {
    ota_status("OTA: firmware preload failed, aborting");
    reportOtaResult(ann, manifest, attempt, "download", "download_failed", error);
    s_busy = false;
    return;
  }
  ota_status("OTA: firmware ready, starting target RX mode");

  ota_status("OTA: sending FANET ACK handshake");
  if(!sendFanetAck(ann)) {
    reportOtaResult(ann, manifest, attempt, "handshake", "fanet_ack_failed");
    s_busy = false;
    return;
  }

  delay(80);
  applyRadioMode(true);
  delay(120);

  // The device checks the signature and erases the whole target area before it
  // answers - several seconds on a SAMD21 - and does not listen meanwhile. So
  // START goes out again until it answers: a repeat to a device that already
  // started its session is answered at once and changes nothing (firmware
  // 1.1.x and later), a repeat after a lost START starts it.
  ota_status("OTA: switching to transfer mode and sending start packet");
  OtaAckInfo ackInfo;
  bool startAcked = false;
  const uint32_t startBeginMs = millis();
  while(!startAcked && millis() - startBeginMs < OTA_GS_START_TOTAL_MS) {
    if(!sendStart(ann, manifest)) {
      applyRadioMode(false);
      reportOtaResult(ann, manifest, attempt, "handshake", "start_send_failed");
      s_busy = false;
      return;
    }
    attempt.startSends++;
    startAcked = waitForAck(ann.nonce, ackInfo, OTA_GS_START_RETRY_MS);
  }
  if(startAcked) attempt.startAckMs = millis() - startBeginMs;
  if(!startAcked || ackInfo.status != OTA_STATE_RX) {
    ota_status("OTA: start handshake not accepted by device");
    sendAbort(ann, OTA_ERR_TIMEOUT);
    applyRadioMode(false);
    reportOtaResult(ann, manifest, attempt, "handshake", "start_not_acked",
                    startAcked ? String("status=0x") + String(ackInfo.status, HEX) : String("timeout"));
    s_busy = false;
    return;
  }

  ota_status("OTA: handshake successful, transferring firmware");
  const uint32_t transferStartMs = millis();
  // The sensor's answer to START names the chunk it expects next: 0 for a
  // fresh session, more when it continues an interrupted transfer of this
  // image (see OTA_START_FLAG_RESUME). Anything beyond the image is not an
  // answer this station can use; the transfer then fails on the first chunk
  // and the next session starts clean.
  uint16_t startSeq = 0u;
  if(ann.versionBcd >= OTA_RESUME_MIN_VERSION_BCD && ackInfo.nextSeq > 0u && ackInfo.nextSeq <= totalChunks) {
    startSeq = ackInfo.nextSeq;
    ota_status(String("OTA: sensor resumes at chunk ") + startSeq + " of " + totalChunks);
  }
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
  const bool transferOk = streamFirmware(ann, manifest, firmwareImage, startSeq, chunkRetries, chunkSnr, chunkAckResends,
                                          chunkDeviceSnr, chunkDeviceRssi, chunkProactiveAckResends);
  postOtaStats(ann, manifest, chunkRetries, chunkSnr, chunkAckResends, chunkDeviceSnr, chunkDeviceRssi, chunkProactiveAckResends, startSeq, transferOk);
  ota_progress_end(transferOk);
  // The last stats entry of a failed transfer is the chunk that was not accepted.
  attempt.chunksSent = startSeq + chunkRetries.size() - ((!transferOk && !chunkRetries.empty()) ? 1u : 0u);
  if(!transferOk) {
    ota_status("OTA: firmware transfer failed");
    sendAbort(ann, OTA_ERR_TIMEOUT);
    applyRadioMode(false);
    reportOtaResult(ann, manifest, attempt, "transfer", "transfer_failed");
    s_busy = false;
    return;
  }

  ota_status(String("OTA: transfer duration ") + String((millis() - transferStartMs) / 1000.0f, 2) + " s");
  //ota_status("OTA: transfer done, sending finish");
  if(!sendFinish(ann, manifest)) {
    sendAbort(ann, OTA_ERR_TIMEOUT);
    applyRadioMode(false);
    reportOtaResult(ann, manifest, attempt, "finish", "finish_send_failed");
    s_busy = false;
    return;
  }

  // The sensor answers FINISH after it has checked the image, a second or two.
  // An ACK that still says RX was sent before FINISH reached it - a proactive
  // resend while it waits, or FINISH got lost - and is no answer: FINISH goes
  // out again. Before, the first such ACK ended the attempt as "rejected", and
  // the sensor sat on a complete image until its session timed out.
  OtaAckInfo finishAck;
  bool finishAnswered = false;
  for(uint8_t send = 0; send < OTA_GS_FINISH_SENDS && !finishAnswered; ++send) {
    if(send > 0u) {
      ota_status("OTA: no answer to finish, sending it again");
      if(!sendFinish(ann, manifest)) {
        break;
      }
    }
    const uint32_t until = millis() + OTA_GS_FINISH_WAIT_MS;
    while(!finishAnswered && (int32_t)(until - millis()) > 0) {
      if(!waitForAck(ann.nonce, finishAck, until - millis())) {
        break;
      }
      finishAnswered = finishAck.status != OTA_STATE_RX;
    }
  }
  const char* finishReason;
  String finishDetail;
  if(finishAnswered && finishAck.status == OTA_STATE_READY) {
    ota_status("OTA: finished successfully");
    finishReason = "success";
    // No more holds for this sensor until the backend was asked again: its
    // next packets come from the new firmware.
    portENTER_CRITICAL(&s_history_lock);
    OtaHistory* done = historySlotLocked(ann.vendor, ann.address);
    done->preloadState = PRELOAD_NONE;
    done->holdAllowed = false;
    done->preloadCheckedMs = 0;
    portEXIT_CRITICAL(&s_history_lock);
  } else if(finishAnswered) {
    ota_status(String("OTA: finish rejected by device, status=0x") + String(finishAck.status, HEX));
    finishReason = "finish_rejected";
    finishDetail = String("status=0x") + String(finishAck.status, HEX);
  } else {
    ota_status("OTA: finish not acknowledged as ready");
    finishReason = "finish_not_ready";
  }

  applyRadioMode(false);
  reportOtaResult(ann, manifest, attempt, "finish", finishReason, finishDetail);
  s_busy = false;
}

#else

void ota_gs_begin() {}
bool ota_gs_hold(const hwInfoData& info) { (void)info; return false; }
void ota_gs_try_update(const hwInfoData& info) { (void)info; }
void ota_gs_note_hwinfo(const hwInfoData& info) { (void)info; }
void ota_gs_preload_tick() {}
void ota_gs_history_write(BdJsonOut& out) { (void)out; }
bool ota_gs_busy() { return false; }

#endif
