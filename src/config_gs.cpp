#include "config_gs.h"

#if BREEZEDUDE_GS_ENABLE_CONFIG_UPDATE

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <RadioLib.h>

extern SX1262 radio_sx1262;
extern PhysicalLayer* radio_phy;
extern volatile bool receivedFlag;
extern void webconsole_print(String in);
extern weatherData weatherStore[MAX_DEVICES];
extern trackingData trackingStore[MAX_DEVICES];

#ifndef CONFIG_GS_BACKEND_URL
  #define CONFIG_GS_BACKEND_URL "http://fanet.breezedude.de/api/config"
#endif

// Config update packet type from device
static constexpr uint8_t CONFIG_UPDATE_PKT_TYPE = 0x0C;

static uint32_t s_last_poll_ms = 0;
static constexpr uint32_t POLL_INTERVAL_MS = 60000UL; // Poll every 60 seconds

static void config_status(const String& msg) {
  Serial.println(msg);
  webconsole_print(msg);
}

static bool parseHexBytes(const String& hex, uint8_t* out, size_t maxLen, size_t& actualLen) {
  if(hex.length() % 2 != 0) {
    return false;
  }

  actualLen = hex.length() / 2;
  if(actualLen > maxLen) {
    return false;
  }

  auto fromNibble = [](char c) -> int {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if(c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  };

  for(size_t i = 0; i < actualLen; ++i) {
    const int hi = fromNibble(hex[(int)(i * 2u)]);
    const int lo = fromNibble(hex[(int)(i * 2u + 1u)]);
    if(hi < 0 || lo < 0) {
      return false;
    }
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static String force_http_url(String url) {
  // Ensure http:// prefix for non-HTTPS backend
  if(!url.startsWith("http://") && !url.startsWith("https://")) {
    url = "http://" + url;
  }
  return url;
}

struct ConfigPendingResponse {
  bool pending = false;
  String fanetId;
  uint8_t count = 0;
  String sessionId;
  String packetHex;
  size_t packetLength = 0;
};

static bool queryPendingConfigs(const String& fanetId, ConfigPendingResponse& response) {
  const String url = force_http_url(String(CONFIG_GS_BACKEND_URL) + "/pending/" + fanetId);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  int code = -1;

  // Use WiFiClient for HTTP (not HTTPS)
  WiFiClient client;
  client.setTimeout(10000);

  if(!http.begin(client, url)) {
    config_status("Config: backend begin failed for " + fanetId);
    return false;
  }

  code = http.GET();

  if(code != HTTP_CODE_OK) {
    if(code != 404) {  // Don't log 404 as error
      config_status(String("Config: backend HTTP error ") + code + " for " + fanetId);
    }
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if(err) {
    config_status("Config: backend JSON parse failed for " + fanetId);
    return false;
  }

  response.fanetId = fanetId;
  response.pending = doc["pending"] | false;
  response.count = doc["count"] | 0;

  if(!response.pending || response.count == 0) {
    return true;  // Valid response, just no configs pending
  }

  const char* sessionId = doc["session_id"] | "";
  const char* packetHex = doc["packet"]["hex"] | "";
  
  response.sessionId = sessionId;
  response.packetHex = packetHex;
  response.packetLength = doc["packet"]["length"] | 0;

  if(response.sessionId.isEmpty() || response.packetHex.isEmpty()) {
    config_status("Config: backend response incomplete for " + fanetId);
    response.pending = false;
    return false;
  }

  return true;
}

static bool confirmConfigTransmission(const String& sessionId) {
  const String url = force_http_url(String(CONFIG_GS_BACKEND_URL) + "/confirm");

  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  int code = -1;

  WiFiClient client;
  client.setTimeout(10000);

  if(!http.begin(client, url)) {
    config_status("Config: confirm begin failed");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["session_id"] = sessionId;
  String payload;
  serializeJson(doc, payload);

  code = http.POST(payload);

  if(code != HTTP_CODE_OK) {
    config_status(String("Config: confirm HTTP error ") + code);
    http.end();
    return false;
  }

  http.end();
  config_status("Config: transmission confirmed with session " + sessionId.substring(0, 8) + "...");
  return true;
}

// OTA packet prefix for config update packets (must match device-side OTA_OP_CONFIG)
static constexpr uint8_t CONFIG_OTA_MAGIC0  = 'O';
static constexpr uint8_t CONFIG_OTA_MAGIC1  = 'T';
static constexpr uint8_t CONFIG_OTA_PROTO   = 3u;
static constexpr uint8_t CONFIG_OTA_OP      = 0x06u; // OTA_OP_CONFIG on device
static constexpr uint8_t CONFIG_OTA_OP_ACK  = 0x04u; // OTA_OP_ACK
static constexpr uint8_t CONFIG_OTA_IDLE    = 0x00u; // OTA_STATE_IDLE = success
static constexpr uint32_t CONFIG_ACK_TIMEOUT_MS = 4000UL;

struct __attribute__((packed)) config_ota_ack_pkt_t {
  uint8_t magic0;
  uint8_t magic1;
  uint8_t proto;
  uint8_t op;
  uint32_t nonce;
  uint16_t next_seq;
  uint8_t status;
  uint8_t accepted_chunk;
};

// Wait for device ACK after sending a config packet.
// nonce=0 means accept any nonce (used when nonce is unknown).
static bool waitForConfigAck(uint32_t nonce, uint8_t& outStatus, uint32_t timeoutMs) {
  const uint32_t deadline = millis() + timeoutMs;
  while(millis() < deadline) {
    if(receivedFlag) {
      receivedFlag = false;
      int numBytes = radio_phy->getPacketLength();
      if(numBytes < (int)sizeof(config_ota_ack_pkt_t) || numBytes > 255) {
        radio_phy->startReceive();
        continue;
      }
      uint8_t buf[255];
      int state = radio_phy->readData(buf, numBytes);
      if(state != RADIOLIB_ERR_NONE) {
        radio_phy->startReceive();
        continue;
      }
      const config_ota_ack_pkt_t* ack = (const config_ota_ack_pkt_t*)buf;
      if(ack->magic0 != CONFIG_OTA_MAGIC0 || ack->magic1 != CONFIG_OTA_MAGIC1 ||
         ack->proto != CONFIG_OTA_PROTO   || ack->op != CONFIG_OTA_OP_ACK) {
        radio_phy->startReceive();
        continue;
      }
      if(nonce != 0u && ack->nonce != nonce) {
        radio_phy->startReceive();
        continue;
      }
      outStatus = ack->status;
      return true;
    }
    delay(1);
  }
  return false;
}

static bool transmitConfigPacket(const uint8_t* payload, size_t payloadLen) {
  // Device expects: [OTA_MAGIC0='O'][OTA_MAGIC1='T'][proto=3][op=0x06] + payload
  // buildConfigUpdatePacket() returns raw payload starting with 0x0C — prepend prefix here.
  static constexpr size_t PREFIX_LEN = 4u;
  const size_t totalLen = PREFIX_LEN + payloadLen;

  if(totalLen > 256u) {
    config_status("Config: packet too large to transmit");
    return false;
  }

  uint8_t buf[256];
  buf[0] = CONFIG_OTA_MAGIC0;
  buf[1] = CONFIG_OTA_MAGIC1;
  buf[2] = CONFIG_OTA_PROTO;
  buf[3] = CONFIG_OTA_OP;
  memcpy(buf + PREFIX_LEN, payload, payloadLen);

  config_status(String("Config: transmitting ") + totalLen + " bytes via LoRa");

  int state = radio_phy->transmit(buf, totalLen);
  if(state != RADIOLIB_ERR_NONE) {
    config_status(String("Config: radio TX failed: ") + state);
    return false;
  }

  receivedFlag = false;
  radio_phy->startReceive();
  return true;
}

static void processConfigUpdate(const String& fanetId, uint32_t nonce = 0u) {
  // Loop until all pending configs for this device are confirmed.
  // The backend may track each setting as a separate session, so after one
  // confirm the next poll immediately re-checks rather than waiting 60 seconds.
  for(int attempt = 0; attempt < 16; ++attempt) {
    ConfigPendingResponse response;

    if(!queryPendingConfigs(fanetId, response)) {
      return;
    }

    if(!response.pending) {
      return;  // No (more) config updates pending
    }

    config_status(String("Config: ") + response.count + " config(s) pending for " + fanetId);

    // Parse hex packet
    uint8_t packet[256];
    size_t packetLen = 0;
    if(!parseHexBytes(response.packetHex, packet, sizeof(packet), packetLen)) {
      config_status("Config: failed to parse hex packet");
      return;
    }

    if(packetLen != response.packetLength) {
      config_status(String("Config: packet length mismatch ") + packetLen + " vs " + response.packetLength);
      return;
    }

    // Verify packet type
    if(packetLen < 1 || packet[0] != CONFIG_UPDATE_PKT_TYPE) {
      config_status(String("Config: invalid packet type 0x") + String(packet[0], HEX));
      return;
    }

    // Transmit via LoRa
    if(!transmitConfigPacket(packet, packetLen)) {
      config_status("Config: transmission failed for " + fanetId);
      return;
    }

    config_status("Config: packet transmitted successfully for " + fanetId);

    // Wait for device ACK — only confirm to backend if device reports success
    uint8_t ackStatus = 0xFFu;
    if(!waitForConfigAck(nonce, ackStatus, CONFIG_ACK_TIMEOUT_MS)) {
      config_status("Config: no ACK from device for " + fanetId);
      return;
    }
    if(ackStatus != CONFIG_OTA_IDLE) {
      config_status(String("Config: device rejected config, status=0x") + String(ackStatus, HEX) + " for " + fanetId);
      return;
    }

    // Confirm to backend only after device confirmed successful application
    if(!confirmConfigTransmission(response.sessionId)) {
      config_status("Config: failed to confirm to backend");
      return;
    }

    config_status("Config: update confirmed for " + fanetId + " (session " + response.sessionId.substring(0, 8) + "...)");

    // If the backend reported more pending configs, loop immediately to send them.
    // Add a short pause so the device has time to finish rebooting (it resets after apply).
    if(response.count <= 1) {
      config_status("Config: all updates complete for " + fanetId);
      return;
    }
    config_status(String("Config: ") + (response.count - 1) + " more pending, continuing...");
    delay(3000); // device reboots after apply; wait before next send
  }
}

static String getUniqueDeviceIds(String* deviceIds, size_t maxDevices) {
  size_t count = 0;
  
  // Collect unique device IDs from weather stations
  for(int i = 0; i < MAX_DEVICES && count < maxDevices; i++) {
    if(weatherStore[i].timestamp == 0 || weatherStore[i].vid == 0) {
      continue;
    }
    
    char devId[16];
    snprintf(devId, sizeof(devId), "%02X%04X", weatherStore[i].vid, weatherStore[i].fanet_id);
    String id = String(devId);
    
    // Check if already in list
    bool exists = false;
    for(size_t j = 0; j < count; j++) {
      if(deviceIds[j] == id) {
        exists = true;
        break;
      }
    }
    
    if(!exists) {
      deviceIds[count++] = id;
    }
  }
  
  // Collect unique device IDs from tracking stations  
  for(int i = 0; i < MAX_DEVICES && count < maxDevices; i++) {
    if(trackingStore[i].timestamp == 0 || trackingStore[i].vid == 0) {
      continue;
    }
    
    char devId[16];
    snprintf(devId, sizeof(devId), "%02X%04X", trackingStore[i].vid, trackingStore[i].fanet_id);
    String id = String(devId);
    
    // Check if already in list
    bool exists = false;
    for(size_t j = 0; j < count; j++) {
      if(deviceIds[j] == id) {
        exists = true;
        break;
      }
    }
    
    if(!exists) {
      deviceIds[count++] = id;
    }
  }
  
  return String(count);
}

void config_gs_begin() {
  s_last_poll_ms = 0;
  config_status("Config: update service initialized");
}

void config_gs_poll_devices() {
  if(WiFi.status() != WL_CONNECTED) {
    return;
  }

  const uint32_t now = millis();
  if((now - s_last_poll_ms) < POLL_INTERVAL_MS) {
    return;
  }

  s_last_poll_ms = now;

  // Get list of all known devices
  String deviceIds[MAX_DEVICES * 2];  // Both weather and tracking
  size_t deviceCount = 0;
  
  getUniqueDeviceIds(deviceIds, MAX_DEVICES * 2);

  // Poll each device for pending configs
  for(int i = 0; i < MAX_DEVICES; i++) {
    if(weatherStore[i].timestamp == 0 || weatherStore[i].vid == 0) {
      continue;
    }
    
    char devId[16];
    snprintf(devId, sizeof(devId), "%02X%04X", weatherStore[i].vid, weatherStore[i].fanet_id);
    
    processConfigUpdate(String(devId));
    delay(100);  // Small delay between devices
  }
  
  for(int i = 0; i < MAX_DEVICES; i++) {
    if(trackingStore[i].timestamp == 0 || trackingStore[i].vid == 0) {
      continue;
    }
    
    char devId[16];
    snprintf(devId, sizeof(devId), "%02X%04X", trackingStore[i].vid, trackingStore[i].fanet_id);
    
    processConfigUpdate(String(devId));
    delay(100);  // Small delay between devices
  }
}

void config_gs_check_device(const String& deviceId, uint32_t nonce) {
  if(WiFi.status() != WL_CONNECTED) {
    return;
  }
  processConfigUpdate(deviceId, nonce);
}

#else

void config_gs_begin() {}
void config_gs_poll_devices() {}
void config_gs_check_device(const String& deviceId) { (void)deviceId; }

#endif
