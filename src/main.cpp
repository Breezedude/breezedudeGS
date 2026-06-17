#include <Arduino.h>
#include <RadioLib.h>
#include <Preferences.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include "FS.h"
#include <LittleFS.h>
#include <esp32FOTA.hpp> // https://github.com/chrisjoyce911/esp32FOTA/tree/master

#include <DNSServer.h>
#include <time.h>

#include "helper.h"
#include "types.h"
#include "ws.h"
#include "aprs.h"
#include "ota_gs.h"
#include "config_gs.h"
#include "recovery.h"
#include "power_management.h"

#define SPIFFS LittleFS

#ifndef BREEZEDUDE_RADIO_SX1276
  #define BREEZEDUDE_RADIO_SX1276 1
#endif
#ifndef BREEZEDUDE_RADIO_LLCC68
  #define BREEZEDUDE_RADIO_LLCC68 1
#endif
#ifndef BREEZEDUDE_RADIO_SX1262
  #define BREEZEDUDE_RADIO_SX1262 1
#endif

/* This code uses "quick re-define" of SPIFFS to run
   an existing sketch with LittleFS instead of SPIFFS

   You only need to format LittleFS the first time you run a
   test or else use the LittleFS plugin to create a partition
   https://github.com/lorol/arduino-esp32littlefs-plugin */

#define FORMAT_LITTLEFS_IF_FAILED true

#define PIN_LORA_MISO 11  // 
#define PIN_LORA_MOSI 10  // 
#define PIN_LORA_SCK 9    // 

#define PIN_LORA_CS 8     // 
#define PIN_LORA_RESET 12 // 
#define PIN_LORA_DIO1 14  // PIN_LORA_IRQ/DIO0 on SX1276
#define PIN_LORA_BUSY 13  // DIO1 on SX1276
#define LORA_SYNCWORD 0xF1 //SX1262: 0xF4 0x14 https://blog.classycode.com/lora-sync-word-compatibility-between-sx127x-and-sx126x-460324d1787a is handled by RadioLib

#define PIN_LED 35
#define PIN_VEXT_CTRL 36 // switches VEXT. Pulled up. Not used currently
#define PIN_ADC_CTRL 37 // VBAT measurement control pin, set to HIGH to enable voltage divider and read battery voltage on ADC pin, set to LOW to save power when not measuring.
#define PIN_ADC_IN 1 // Analog in for battery voltage measurement. Connected to Voltage divider 100k/390k
#define PIN_USERBTN 0


String fw_version = FW_VERSION;
bool update_available = false;
String update_available_version = "";
bool force_update_check = false;
uint32_t last_updatecheck = 0;

// for automatic updates, echeckt on connect and every 6 hours
esp32FOTA fotaStable("bd-gs-stable", fw_version, false, true);
esp32FOTA fotaBeta("bd-gs-beta", fw_version, false, true);
esp32FOTA* activeFota = &fotaStable;
const char* manifest_url = "http://fanet.breezedude.de/api/ota/gs-manifest"; //old "https://install.breezedude.de/gs-update.json";

const char* getFotaBranchName() {
  return (strcmp(settings.updateBranch, "beta") == 0) ? "bd-gs-beta" : "bd-gs-stable";
}

void applyFotaBranch() {
  activeFota = (strcmp(settings.updateBranch, "beta") == 0) ? &fotaBeta : &fotaStable;
}

void onUpdateBranchChanged() {
  applyFotaBranch();
  force_update_check = true;
}

SX1276 radio_sx1276 = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RESET, RADIOLIB_NC);
LLCC68 radio_llcc68 = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RESET, PIN_LORA_BUSY);
SX1262 radio_sx1262 = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RESET, PIN_LORA_BUSY);
RadioModuleType lora_module = RADIO_MODULE_NONE;
PhysicalLayer* radio_phy = nullptr;

volatile bool receivedFlag = false;

ICACHE_RAM_ATTR void setFlag(void) {
  receivedFlag = true;
}

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 0;

String breezedudeUrl = "http://fanet.breezedude.de/submit_weather";
String breezedudeHwInfoUrl = "http://fanet.breezedude.de/submit_hwinfo";
Preferences preferences;

Settings settings;

Aprs aprs;
uint32_t fanet_rx_count =0;

DNSServer dnsServer;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
bool littlefsMounted = false;

bool init_lora_radio() {
#if BREEZEDUDE_RADIO_SX1276
  if (radio_sx1276.begin(868.2f, 250.0f, 7, 5, LORA_SYNCWORD, 10, 12, 0) == RADIOLIB_ERR_NONE) {
    radio_phy = (PhysicalLayer*)&radio_sx1276;
    lora_module = RADIO_MODULE_SX1276;
    Serial.println("Radio: LoRa SX1276");
    return true;
  }
#endif

#if BREEZEDUDE_RADIO_LLCC68
  if (radio_llcc68.begin(868.2f, 250.0f, 7, 5, LORA_SYNCWORD, 10, 12) == RADIOLIB_ERR_NONE) {
    radio_phy = (PhysicalLayer*)&radio_llcc68;
    lora_module = RADIO_MODULE_LLCC68;
    radio_phy->setOutputPower(22);
    static_cast<SX126x*>(radio_phy)->setRegulatorDCDC();
    Serial.println("Radio: LoRa LLCC68");
    return true;
  }
#endif

#if BREEZEDUDE_RADIO_SX1262
  if (radio_sx1262.begin(868.2f, 250.0f, 7, 5, LORA_SYNCWORD, 10, 12) == RADIOLIB_ERR_NONE) {
    radio_phy = (PhysicalLayer*)&radio_sx1262;
    lora_module = RADIO_MODULE_SX1262;
    radio_phy->setOutputPower(22);
    static_cast<SX126x*>(radio_phy)->setRegulatorDCDC();
    Serial.println("Radio: LoRa SX1262");
    return true;
  }
#endif

  radio_phy = nullptr;
  lora_module = RADIO_MODULE_NONE;
  return false;
}

void configure_lora_radio(bool fastMode) {
  if (radio_phy == nullptr) {
    return;
  }

  radio_phy->standby();
  radio_phy->setFrequency(868.2f);

  switch (lora_module) {
    case RADIO_MODULE_SX1276:
      radio_sx1276.setBandwidth((float)(fastMode ? OTA_GS_FAST_BW_KHZ : 250));
      radio_sx1276.setSpreadingFactor(fastMode ? OTA_GS_FAST_SF : 7);
      radio_sx1276.setCodingRate(fastMode ? OTA_GS_FAST_CR : 5);
      radio_sx1276.setSyncWord(fastMode ? OTA_GS_FAST_SYNCWORD : LORA_SYNCWORD);
      radio_sx1276.setPreambleLength(fastMode ? OTA_GS_FAST_PREAMBLE : 12);
      break;
    case RADIO_MODULE_LLCC68:
      radio_llcc68.setBandwidth((float)(fastMode ? OTA_GS_FAST_BW_KHZ : 250));
      radio_llcc68.setSpreadingFactor(fastMode ? OTA_GS_FAST_SF : 7);
      radio_llcc68.setCodingRate(fastMode ? OTA_GS_FAST_CR : 5);
      radio_llcc68.setSyncWord(fastMode ? OTA_GS_FAST_SYNCWORD : LORA_SYNCWORD);
      radio_llcc68.setPreambleLength(fastMode ? OTA_GS_FAST_PREAMBLE : 12);
      break;
    case RADIO_MODULE_SX1262:
      radio_sx1262.setBandwidth((float)(fastMode ? OTA_GS_FAST_BW_KHZ : 250));
      radio_sx1262.setSpreadingFactor(fastMode ? OTA_GS_FAST_SF : 7);
      radio_sx1262.setCodingRate(fastMode ? OTA_GS_FAST_CR : 5);
      radio_sx1262.setSyncWord(fastMode ? OTA_GS_FAST_SYNCWORD : LORA_SYNCWORD);
      radio_sx1262.setPreambleLength(fastMode ? OTA_GS_FAST_PREAMBLE : 12);
      break;
    default:
      break;
  }

  receivedFlag = false;
  radio_phy->startReceive();
}


extern weatherData weatherStore[MAX_DEVICES];
extern trackingData trackingStore[MAX_DEVICES];

bool wifiConnected = false;
unsigned long wifiLastAttempt = 0;
const unsigned long wifiRetryInterval = 10000; // retry every 10 sec
// Start with an immediate STA connect attempt right after boot.
bool forceReconnectSTA = true;
bool restartAP = false;
bool wifiGaveUp = false; // kept for ABI compat, no longer used in logic
volatile bool otaUploadInProgress = false;
volatile bool otaUploadRejected = false;

static float filteredBatteryVoltage = NAN;
static float lastBatteryVoltage = NAN;
static uint32_t lastBatteryMeasurementMs = 0;
static bool batteryMeasurementSettled = false;
static uint32_t lastBatterySleepCheckMs = 0;

static HardwareSerial debugUart0(0);

static const size_t WEB_CONSOLE_HISTORY_SIZE = 20;
struct WebConsoleEntry { time_t ts; String msg; };
WebConsoleEntry webconsole_history[WEB_CONSOLE_HISTORY_SIZE];
size_t webconsole_history_count = 0;
size_t webconsole_history_head = 0;

static void webconsole_history_push(time_t ts, const String& msg) {
  if (msg.length() == 0) {
    return;
  }

  webconsole_history[webconsole_history_head] = { ts, msg };
  webconsole_history_head = (webconsole_history_head + 1) % WEB_CONSOLE_HISTORY_SIZE;
  if (webconsole_history_count < WEB_CONSOLE_HISTORY_SIZE) {
    webconsole_history_count++;
  }
}

// Normalize log payload in a single pass to avoid repeated String scans.
static String sanitizeLogLine(const String& input) {
  String out;
  out.reserve(input.length());

  bool sawNonWhitespace = false;
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '\r' || c == '\n') {
      c = ' ';
    }

    if (!sawNonWhitespace) {
      if (c == ' ' || c == '\t') {
        continue;
      }
      sawNonWhitespace = true;
    }

    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) {
      c = '?';
    }

    out += c;
  }

  while (out.length() > 0) {
    char c = out[out.length() - 1];
    if (c == ' ' || c == '\t') {
      out.remove(out.length() - 1);
    } else {
      break;
    }
  }

  return out;
}


void webconsole_print(String in){
  in = sanitizeLogLine(in);
  if (in.length() == 0) {
    return;
  }

  time_t ts = time(nullptr);
  webconsole_history_push(ts, in);

  if(ws.availableForWriteAll()){
    JsonDocument doc;
    doc["webconsole"] = in;
    doc["ts"] = (uint32_t)ts;
    String text;
    serializeJson(doc, text);
    ws.textAll(text);
  }
}

// Verbose OTA logging: set by the web UI per-session via the "set_ota_verbose"
// command and reset whenever the websocket connection drops (see onWsEvent in
// ws.h), so it always has to be explicitly re-enabled. Verbose messages are
// pushed live to connected clients only - they are NOT added to the
// webconsole history buffer, to avoid filling it with per-packet noise.
bool ota_verbose_ws = false;

void ota_status_v(const String& msg){
  if(!ota_verbose_ws) {
    return;
  }
  String in = sanitizeLogLine(msg);
  if (in.length() == 0) {
    return;
  }

  if(ws.availableForWriteAll()){
    JsonDocument doc;
    doc["webconsole"] = in;
    doc["ts"] = (uint32_t)time(nullptr);
    String text;
    serializeJson(doc, text);
    ws.textAll(text);
  }
}

void aprsconsole_print(String in){
  in = sanitizeLogLine(in);
  if (in.length() == 0) {
    return;
  }

  if(ws.availableForWriteAll()){
    JsonDocument doc;
    doc["aprsconsole"] = in;
    String text;
    serializeJson(doc, text);
    ws.textAll(text);
  }
}

// Live OTA chunk-transfer progress pushed to the web UI (see www/app.js,
// section "Tools -> Console -> OTA"). Three phases:
//  - "start": transfer begins, total chunk count is known
//  - "chunk": one chunk has been acked (or permanently failed), with its
//             retry count (15 = transfer-aborting chunk), the SNR seen by
//             the GS for that chunk's ack, the device's ack_resend_count
//             (how often the device resent its ack for this chunk before the
//             GS received it, with proactiveAckResends as the subset of
//             those resends that were sent proactively), and the SNR/RSSI
//             the device measured for the chunk packet itself
//  - "end":   transfer finished (success or failure)
void ota_progress_start(uint32_t totalChunks){
  if(!ws.availableForWriteAll()) return;
  JsonDocument doc;
  JsonObject p = doc["ota_progress"].to<JsonObject>();
  p["phase"] = "start";
  p["total"] = totalChunks;
  String text;
  serializeJson(doc, text);
  ws.textAll(text);
}

void ota_progress_chunk(uint16_t seq, uint8_t retries, float snr, uint8_t ackResendCount,
                         int8_t deviceSnr, int8_t deviceRssi, uint8_t proactiveAckResendCount){
  if(!ws.availableForWriteAll()) return;
  JsonDocument doc;
  JsonObject p = doc["ota_progress"].to<JsonObject>();
  p["phase"] = "chunk";
  p["seq"] = seq;
  p["retries"] = retries;
  p["snr"] = snr;
  p["ackResends"] = ackResendCount;
  p["deviceSnr"] = deviceSnr;
  p["deviceRssi"] = deviceRssi;
  p["proactiveAckResends"] = proactiveAckResendCount;
  String text;
  serializeJson(doc, text);
  ws.textAll(text);
}

void ota_progress_end(bool success){
  if(!ws.availableForWriteAll()) return;
  JsonDocument doc;
  JsonObject p = doc["ota_progress"].to<JsonObject>();
  p["phase"] = "end";
  p["success"] = success;
  String text;
  serializeJson(doc, text);
  ws.textAll(text);
}

void webconsole_sync_client(AsyncWebSocketClient *client) {
  if (client == nullptr || webconsole_history_count == 0) {
    return;
  }

  JsonDocument doc;
  JsonArray arr = doc["webconsole_history"].to<JsonArray>();

  size_t start = (webconsole_history_head + WEB_CONSOLE_HISTORY_SIZE - webconsole_history_count) % WEB_CONSOLE_HISTORY_SIZE;
  for (size_t i = 0; i < webconsole_history_count; i++) {
    size_t idx = (start + i) % WEB_CONSOLE_HISTORY_SIZE;
    JsonObject entry = arr.add<JsonObject>();
    entry["ts"] = (uint32_t)webconsole_history[idx].ts;
    entry["msg"] = sanitizeLogLine(webconsole_history[idx].msg);
  }

  String output;
  serializeJson(doc, output);
  client->text(output);
}

  String getDeviceName(uint8_t vid, uint16_t fanet_id) {
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (weatherStore[i].timestamp != 0 &&
          weatherStore[i].vid == vid &&
          weatherStore[i].fanet_id == fanet_id) {
        return weatherStore[i].name;
      }

      if (trackingStore[i].timestamp != 0 &&
          trackingStore[i].vid == vid &&
          trackingStore[i].fanet_id == fanet_id) {
        return trackingStore[i].name;
      }
    }

    return "";
  }

void setDeviceName(uint8_t vid, uint16_t fanet_id, String name) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (weatherStore[i].timestamp != 0 &&
            weatherStore[i].vid == vid &&
            weatherStore[i].fanet_id == fanet_id) {
            
            weatherStore[i].name = name;
            Serial.printf("Name for vid: %02X, fanet_id: %04X set to %s\n", vid, fanet_id, name.c_str());
            return;
        }
    }
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (trackingStore[i].timestamp != 0 &&
            trackingStore[i].vid == vid &&
            trackingStore[i].fanet_id == fanet_id) {
            
            trackingStore[i].name = name;
            Serial.printf("Name for vid: %02X, fanet_id: %04X set to %s\n", vid, fanet_id, name.c_str());
            return;
        }
    }
    Serial.printf("Device with vid: %02X, fanet_id: %04X not found, cannot set name.\n", vid, fanet_id);
}


void handle_fanet(){
  if (radio_phy == nullptr) {
    return;
  }

  if(receivedFlag) {
    receivedFlag = false;
      
    int numBytes = radio_phy->getPacketLength();
    byte byteArr[numBytes];
    int state = radio_phy->readData(byteArr, numBytes);
    
    if (state == RADIOLIB_ERR_NONE) {
      fanet_rx_count ++;

     // Serial.println(F("[SX1262] Received packet!"));
     //  Serial.print(F("[SX1262] Data:\t\t"));
     //  for ( int i =0; i< numBytes; i++){
     //    Serial.printf("%02X ", byteArr[i]);
     //  }
     //  Serial.println();

    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println(F("CRC error!"));
      return;
    } else {
      Serial.print(F("failed, code "));
      Serial.println(state);
      return;
    }

    fanet_header *header = (fanet_header *)byteArr;
    if(header->type == FANET_PCK_TYPE_WEATHER){
      weatherData wd;
      if(unpack_weatherdata(byteArr, (size_t)numBytes, &wd, radio_phy->getSNR(), radio_phy->getRSSI())){
        //print_weatherData(&wd);
        int i = storeWeatherData(wd);
        if (millis() - weatherStore[i].last_send > 3000){ // filter forwareded duplicates
          weatherStore[i].last_send = millis()+2;
          ws.textAll(packWeather(&weatherStore[i]));

          // check if distance is withing 300km
          if(distance(settings.latitude, settings.longitude, weatherStore[i].lat, weatherStore[i].lon) < 300){
            if(wifiConnected && settings.sendBreezedude){
              send_wd_to_frontend(&weatherStore[i]);
            }
            if(settings.sendAPRS && aprs.connected()){
              if(!aprs.sendWeatherData(&wd)){
                Serial.println(F("APRS submit weather failed"));
              }
            } else {
              Serial.println(F("APRS not connected"));
            }
          }
        }
      }
    }

    else if(header->type == FANET_PCK_TYPE_NAME){
      char name [numBytes-3];
      memset(name,'\0', numBytes-3);
      memcpy(name, &byteArr[4], numBytes-4);
      setDeviceName(header->vendor, header->address, name);
      // filter forwarded duplicates (3s window per device)
      static struct { uint8_t vid; uint16_t fid; uint32_t last_send; } nameDedup[MAX_DEVICES] = {};
      uint32_t now_ms = millis();
      int slot = -1, free_slot = -1;
      for(int i = 0; i < MAX_DEVICES; i++){
        if(nameDedup[i].last_send && nameDedup[i].vid == header->vendor && nameDedup[i].fid == header->address){ slot = i; break; }
        if(!nameDedup[i].last_send && free_slot < 0) free_slot = i;
      }
      bool name_is_dup = (slot >= 0) && (now_ms - nameDedup[slot].last_send < 3000);
      if(!name_is_dup){
        int use = (slot >= 0) ? slot : free_slot;
        if(use >= 0) nameDedup[use] = {header->vendor, header->address, now_ms};
        if(settings.sendAPRS && aprs.connected()){
          aprs.sendNameData(FANET2String(header->vendor, header->address),name,radio_phy->getSNR());
        }
      }
    }
    else if(header->type == FANET_PCK_TYPE_HW_INFO){
      hwInfoData hi;

      /*
      // Print raw hex to webconsole
      String rxHex;
      rxHex.reserve(numBytes * 3 + 16);
      rxHex = "HWINFO RX: ";
      char hexByte[4];
      for (int i = 0; i < numBytes; i++) {
        snprintf(hexByte, sizeof(hexByte), "%02X ", byteArr[i]);
        rxHex += hexByte;
      }
      webconsole_print(rxHex);
      */

      if(unpack_hwinfo_t0a(byteArr, numBytes, &hi, radio_phy->getRSSI(), radio_phy->getSNR())){
        int hi_idx = storeHwInfoData(hi);
        if(millis() - hwInfoStore[hi_idx].last_send > 3000){ // filter forwarded duplicates
          hwInfoStore[hi_idx].last_send = millis();
          ws.textAll(packHwInfo(&hi));
          if(wifiConnected && settings.sendBreezedude){
            send_hwinfo_to_frontend(&hi);
          }
          // Only HW_INFO type 2 guarantees the sender remains in RX mode afterwards.
          if(hi.rawAfterBuildDate[0] == 2) {
            ota_gs_try_update(hi);
            config_gs_try_update(hi);
          }
        }
      }
    }
    else if(header->type == FANET_PCK_TYPE_TRACKING){
      trackingData td;
      if(unpack_trackingdata(byteArr, &td, radio_phy->getSNR(), radio_phy->getRSSI())){
        int i = storeTrackingData(td); // sort and get position
        if (millis() - trackingStore[i].last_send > 800){ // filter forwareded duplicates
            trackingStore[i].last_send = millis();
          ws.textAll(packTracking(&trackingStore[i])); // send complete set including name
          if(aprs.connected()){
            aprs.sendTrackingData(&td);
          }
        }
      }
    } else if(header->type == FANET_PCK_TYPE_GROUND_TRACKING){
      trackingData td;
      if(unpack_ground_trackingdata(byteArr, &td, radio_phy->getSNR(), radio_phy->getRSSI())){
        int i = storeTrackingData(td); // sort and get position
        if (millis() - trackingStore[i].last_send > 800){ // filter forwareded duplicates
            trackingStore[i].last_send = millis();
          ws.textAll(packTracking(&trackingStore[i])); // send complete set including name
        // if(aprs.connected()){
        //   aprs.sendTrackingData(&td);
        // }
        }
      }
    }
  }
}

void server_setup(){

  const char* redirectUrl = "http://192.168.4.1/";
  const char* redirectPaths[] = { "/generate_204", "/gen_204", "/ncsi.txt" };

  for (const char* path : redirectPaths) {
      server.on(path, HTTP_GET, [redirectUrl](AsyncWebServerRequest *request) {
          request->redirect(redirectUrl);
      });
  }

  server.onNotFound([redirectUrl](AsyncWebServerRequest *request) {
      request->redirect(redirectUrl);
  });

  registerRecoveryRoutes(server, ws, littlefsMounted, otaUploadInProgress, otaUploadRejected);
}

String normalize_online_version(const String& versionRaw) {
  int firstDot = versionRaw.indexOf('.');
  int secondDot = versionRaw.indexOf('.', firstDot + 1);
  if (firstDot < 0 || secondDot < 0) {
    return versionRaw;
  }

  String patch = versionRaw.substring(secondDot + 1);
  if (patch.length() <= 1) {
    return versionRaw;
  }

  for (size_t i = 0; i < patch.length(); i++) {
    if (patch[i] < '0' || patch[i] > '9') {
      return versionRaw;
    }
  }

  // Some manifests append a 5-digit build marker to the patch (e.g. 0.6.215003).
  if (patch.length() > 5) {
    String normalizedPatch = patch.substring(0, patch.length() - 5);
    if (normalizedPatch.length() > 0) {
      return versionRaw.substring(0, secondDot + 1) + normalizedPatch;
    }
  }

  return versionRaw;
}

void check_update(){
  if (force_update_check || (last_updatecheck == 0) || (millis() - last_updatecheck > 6*3600*1000)){
    force_update_check = false;
    last_updatecheck = millis();
    webconsole_print("Checking GS firmware updates (branch: " + String(getFotaBranchName()) + ")");
    bool updatedNeeded = activeFota->execHTTPcheck();
    char buff[16];
    memset(buff,'\0', sizeof(buff));
    activeFota->getPayloadVersion(buff);
    String onlineVersionRaw = String(buff);
    String onlineVersionDisplay = normalize_online_version(onlineVersionRaw);
    update_available_version = onlineVersionDisplay;
    update_available = updatedNeeded;
    if (updatedNeeded){
      if (settings.autoUpdate) {
        Serial.println("Installing update over internet");
        webconsole_print("Update available: " + onlineVersionDisplay + " (current " + fw_version + "). Starting Update...");
        activeFota->execOTA();
        webconsole_print("Update execution finished. Device may reboot if update succeeded.");
      } else {
        Serial.printf("Update available: current %s, online %s", fw_version, onlineVersionDisplay.c_str());
        webconsole_print("Update available: " + onlineVersionDisplay + " (current " + fw_version + ") Auto update disabled, need manually install update.");
      }
    } else {
      Serial.printf("Firmware Current Version: %s, Online Version %s\r\n", fw_version, onlineVersionDisplay.c_str());
      webconsole_print("GS Firmware up to date: " + fw_version + " (online " + onlineVersionDisplay + ")");
    }
  }
}

void run_wifi() {
  static uint32_t lastcheck = 0;
  static int connect_error_count =0;
  static bool ap_started = false;
  static bool server_started = false;
  if(millis()-lastcheck > 300){
    lastcheck = millis();

    bool apEnabled = settings.keepAP || (WiFi.status() != WL_CONNECTED); // enable ap if not connected or AP should keep running
    if (apEnabled) {
        if (WiFi.getMode() != WIFI_AP_STA) {
            Serial.println("Switching to AP+STA mode");
            WiFi.mode(WIFI_AP_STA);
        }
        if (!ap_started) {
            // If AP is not running, start it
            if (!WiFi.softAP(settings.ap_ssid, settings.ap_password)) {
                Serial.println("Failed to start AP");
                ap_started = true; // avoid error loop
            } else {
                Serial.println("AP started: " + String(settings.ap_ssid));
                Serial.println("AP IP: " + WiFi.softAPIP().toString());

                IPAddress apIP = WiFi.softAPIP();
                dnsServer.start(53, "*", apIP);
                ap_started = true;
            }
        }
    } else {
        if (WiFi.getMode() != WIFI_STA) {
            Serial.println("Switching to STA mode");
            WiFi.mode(WIFI_STA);
            ap_started = false;
        }
    }

    if(restartAP){
      WiFi.mode(WIFI_STA);
      ap_started = false;
      restartAP= false;
    }

    // STA connection handling
    if(forceReconnectSTA){
      connect_error_count = 0;
    }
    if (WiFi.status() != WL_CONNECTED && !otaUploadInProgress) {
      wifiConnected = false;
      // After 3 quick retries (every 10 s) back off to one attempt per 60 s;
      // never stop retrying – a brief router restart should not require a
      // manual reboot of the ground station.
      const unsigned long retryInterval = (connect_error_count < 3) ? wifiRetryInterval : 60000UL;
        if (forceReconnectSTA || (millis() - wifiLastAttempt >= retryInterval)) {
            wifiLastAttempt = millis();
            if (connect_error_count < 3) connect_error_count++;
            forceReconnectSTA = false;
            Serial.println("Attempting WiFi connection to SSID: " + String(settings.wifi_ssid));
            WiFi.begin(settings.wifi_ssid, settings.wifi_password);
            //WiFi.persistent(false);
            //WiFi.setAutoConnect(false);
            //WiFi.setAutoReconnect(false);
            //WiFi.setTxPower(WIFI_POWER_17dBm);
        }
    } else if (!otaUploadInProgress) {
        if (!wifiConnected) {
            wifiConnected = true;
            connect_error_count = 0;
        String connectedMsg = "Connected to WiFi, IP: " + WiFi.localIP().toString();
        Serial.println(connectedMsg);
        Serial.println("GS_IP:" + WiFi.localIP().toString());
        webconsole_print(connectedMsg);

        JsonDocument doc;
        doc["msg"] = connectedMsg;
        doc["sta_status"] = "connected";
        doc["sta_ip"] = WiFi.localIP().toString();
        String text;
        serializeJson(doc, text);
        ws.textAll(text);
        }
        updateInternetConnectionStatus();
        check_update();
    }
  }

  process_forward_queue_tick();

if(!server_started){
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    if (littlefsMounted && (LittleFS.exists("/index.html") || LittleFS.exists("/index.html.gz"))) {
      server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    } else {
      Serial.println("LittleFS web UI not available - serving fallback updater page");
    }
    server_setup();
    server.begin();
    ws_init(&ws);
    server_started = true;
}
    // If STA connection fails and AP fallback is needed, it is already handled above with AP+STA mode
}

void load_preferences(){
  preferences.begin("gs_config", true);
  if (preferences.isKey("Settings")) {
    size_t loaded = preferences.getBytes("Settings", &settings, sizeof(settings));
    if (loaded == 0) {
      Serial.println("load_preferences: no stored settings found - using defaults");
    } else if (loaded != sizeof(settings)) {
      Serial.printf("load_preferences: partial settings load (got %u, current size %u) - keeping defaults for new fields\n",
                    (unsigned)loaded, (unsigned)sizeof(settings));
    }
    settings.updateBranch[sizeof(settings.updateBranch) - 1] = '\0';
    if (strcmp(settings.updateBranch, "stable") != 0 && strcmp(settings.updateBranch, "beta") != 0) {
      strncpy(settings.updateBranch, "stable", sizeof(settings.updateBranch));
      settings.updateBranch[sizeof(settings.updateBranch) - 1] = '\0';
    }
  }
  preferences.end();
}

void save_preferences(){
  preferences.begin("gs_config", false);
  size_t loaded = preferences.putBytes("Settings", &settings, sizeof(settings));
  preferences.end();
}

// ── Serial command handler ────────────────────────────────────────────────────
// Accepts: GS_CONFIG:{"ssid":"...","pass":"...","name":"...","lat":47.0,"lon":11.0,"alt":800}\r\n
// Replies: GS_CONFIG:OK, then GS_IP:<ip>  on success
//          GS_CONFIG:OK, then GS_WIFI:FAIL:<reason>  on failure
//          GS_CONFIG:ERROR:<reason>  on parse error
static String _serialBuf;

// Try to connect to WiFi synchronously (up to 3 attempts, 10 s each).
// Returns empty string on success, or a failure reason token.
static String connectWiFiBlocking(const char* ssid, const char* pass) {
  WiFi.disconnect(true);
  delay(300);

  const int    MAX_ATTEMPTS    = 3;
  const unsigned long TIMEOUT_MS = 10000;

  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    Serial.printf("WiFi connect attempt %d/%d to SSID: %s\n", attempt, MAX_ATTEMPTS, ssid);
    WiFi.begin(ssid, pass);

    unsigned long t0 = millis();
    wl_status_t st = WL_IDLE_STATUS;
    bool authFailed = false;
    while (millis() - t0 < TIMEOUT_MS) {
      st = WiFi.status();
      if (st == WL_CONNECTED)     return "";                // success
      if (st == WL_NO_SSID_AVAIL) return "ssid_not_found"; // no point retrying
      if (st == WL_CONNECT_FAILED) { authFailed = true; break; } // try again before reporting
      delay(200);
      yield();
    }

    Serial.printf("WiFi attempt %d failed: status=%d authFail=%d\n", attempt, (int)st, (int)authFailed);
    WiFi.disconnect(true);
    delay(300);

    // Only report auth failure after all attempts to avoid false positives on transient errors
    if (authFailed && attempt == MAX_ATTEMPTS) return "auth_failed";
  }

  return "timeout";
}

static void handleSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      String line = _serialBuf;
      _serialBuf = "";
      line.trim();
      if (!line.startsWith("GS_CONFIG:")) continue;

      String json = line.substring(10);
      JsonDocument doc;
      if (deserializeJson(doc, json)) {
        Serial.println("GS_CONFIG:ERROR:json_parse_failed");
        continue;
      }

      bool changed    = false;
      bool hasNewSsid = doc["ssid"].is<const char*>();

      if (hasNewSsid) {
        strncpy(settings.wifi_ssid,     doc["ssid"] | "", sizeof(settings.wifi_ssid) - 1);
        strncpy(settings.wifi_password, doc["pass"] | "", sizeof(settings.wifi_password) - 1);
        changed = true;
      }
      if (doc["name"].is<const char*>()) {
        strncpy(settings.deviceName, doc["name"] | "", sizeof(settings.deviceName) - 1);
        changed = true;
      }
      if (doc["lat"].is<float>()) { settings.latitude  = doc["lat"].as<float>(); changed = true; }
      if (doc["lon"].is<float>()) { settings.longitude = doc["lon"].as<float>(); changed = true; }
      if (doc["alt"].is<int>())   { settings.elevation = doc["alt"].as<int>();   changed = true; }

      // After applying all fields, check if the station is now fully configured
      // (has WiFi, a non-default name, and a non-default position). If so, enable
      // data forwarding automatically so no manual web-UI step is needed.
      bool hasWifi = strlen(settings.wifi_ssid) > 0;
      bool hasName = strlen(settings.deviceName) > 0 && strcmp(settings.deviceName, "MyGS") != 0;
      bool hasPos  = !(fabsf(settings.latitude  - 47.0f) < 0.001f &&
                       fabsf(settings.longitude - 12.0f) < 0.001f);
      if (hasWifi && hasName && hasPos) {
        settings.sendBreezedude = true;
        settings.sendAPRS       = true;
        changed = true;
      }

      if (changed) save_preferences();
      Serial.setDebugOutput(false);
      Serial.println("GS_CONFIG:OK");
      Serial.flush();
      Serial.setDebugOutput(true);

      if (hasNewSsid) {
        String err = connectWiFiBlocking(settings.wifi_ssid, settings.wifi_password);
        if (err.isEmpty()) {
          wifiConnected     = true;
          forceReconnectSTA = false;
          String ip = WiFi.localIP().toString();
          Serial.println("Connected to WiFi, IP: " + ip);
          Serial.setDebugOutput(false);
          Serial.println("GS_IP:" + ip);
          Serial.flush();
          Serial.setDebugOutput(true);
        } else {
          Serial.setDebugOutput(false);
          Serial.println("GS_WIFI:FAIL:" + err);
          Serial.flush();
          Serial.setDebugOutput(true);
          forceReconnectSTA = true; // keep retrying in background
        }
      }
    } else if (c != '\r') {
      if (_serialBuf.length() < 512) _serialBuf += c;
    }
  }
}

static void initSerialOutputs() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  debugUart0.begin(115200);
  debugUart0.setDebugOutput(true);

  // Keep both outputs active so the same firmware image works on
  // native USB CDC boards and CP2102 UART-USB adapter boards.
  Serial.println("[serial] debug output on USB CDC + UART0");
  debugUart0.println("[serial] debug output on USB CDC + UART0");
}

void setup() {
    pinMode(PIN_USERBTN,INPUT_PULLUP);
    pinMode(PIN_LED,OUTPUT);
    initSerialOutputs();
    initPowerManagementHardware();

    migratePartitionLayoutIfNeeded();
    littlefsMounted = LittleFS.begin();
    if (!littlefsMounted) {
      Serial.println("LittleFS Mount Failed - fallback updater page enabled");
    }

    Serial.println("Press User button for reset");
    delay(1000);
    if(digitalRead(PIN_USERBTN) == LOW){
      digitalWrite(PIN_LED,1);
      save_preferences(); // write default settings
      Serial.println("Reset to default settings");
      delay(200);
      digitalWrite(PIN_LED,0);
    } else {
      load_preferences();
    }

    // On deep-sleep wake with depleted battery, skip bringing up radio/WiFi
    // and go back to sleep immediately.
    handleLowBatteryAtStartup();

    fotaStable.setManifestURL(manifest_url);
    fotaBeta.setManifestURL(manifest_url);
    applyFotaBranch();
    activeFota->printConfig();

    // init buffers
    for (int i = 0; i < MAX_DEVICES; i++) {
      weatherStore[i].timestamp = 0;
      trackingStore[i].timestamp = 0;
    }

  if(!init_lora_radio()){
        Serial.println("Radio not found - continuing without LoRa");
  } else {
    radio_phy->setPacketReceivedAction(setFlag);
    int state = radio_phy->startReceive();
    if (state != RADIOLIB_ERR_NONE) {
          Serial.print(F("radio startReceive failed, code "));
      Serial.println(state);
      radio_phy = nullptr;
      receivedFlag = false;
    }
  }

  WiFi.setHostname(settings.deviceName);

  configureTimeZoneFromLocation();
  setSyncProvider(getNtpTime);
  setSyncInterval(300);
  config_gs_begin();
  ota_gs_begin();
  update_aprs_settings();

  // Signal to the web installer that serial is ready and config can be sent
  Serial.println("GS_READY");
}

void handle_dummy(){
  static uint32_t last_dummy = 0;
  if(millis() - last_dummy > 10000){
    last_dummy = millis();
    weatherData wd;
    fill_weatherData_dummy(&wd);
    int i = storeWeatherData(wd);
    weatherStore[i].name = "Test Station (420m)";
    ws.textAll(packWeather(&weatherStore[i]));
    if(wifiConnected && settings.sendBreezedude){
      send_wd_to_frontend(&weatherStore[i]);
    }
  }
}


uint32_t lastCall = 0;

void loop() {
  dnsServer.processNextRequest();
  handleSerialCommands();

  handleBatteryAndSleep();
  wifi_scan_tick();
  run_wifi();

  if(millis() - lastCall > 5000){
    lastCall = millis();
    Serial.println(millis());
  }

  handle_fanet();
  //handle_dummy();
  if(settings.sendAPRS && !otaUploadInProgress){
    aprs.run(wifiConnected);
  }
}
