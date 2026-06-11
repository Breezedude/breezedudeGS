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
#include <esp_sleep.h>
#include <esp_bt.h>

#include "helper.h"
#include "types.h"
#include "ws.h"
#include "aprs.h"
#include "ota_gs.h"
#include "config_gs.h"
#include "recovery.h"
//#include "improv_serial.h"

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


// Wifi WiFi Sta connected and AP off avg. power consuption is ~ 52mA @4V battery (48mA @ 3,4V)
// deepsleep 980µA
// charge current Heltec Wireless stick V3: Rprog is 2K - > tp4054: 375-340mA, I-term 50mA

static constexpr uint8_t BATTERY_ADC_SAMPLES = 12;
static constexpr uint32_t BATTERY_MEASUREMENT_INTERVAL_MS = 15000UL;
static constexpr float BATTERY_LOW_PASS_ALPHA = 0.18f;
static constexpr float BATTERY_DIVIDER_RATIO = 4.9f;
static constexpr float BATTERY_UNDERVOLTAGE_THRESHOLD = 3.1f; // Volt
static constexpr uint64_t LOW_BATTERY_SLEEP_SECONDS = 12ULL * 60ULL * 60ULL; // 12 hours


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
    entry["msg"] = webconsole_history[idx].msg;
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
    // If the stored blob is a different size (struct changed across firmware
    // updates), discard it and keep the safe default-constructed values.
    if (loaded != sizeof(settings)) {
      Serial.printf("load_preferences: size mismatch (got %u, expected %u) – using defaults\n",
                    (unsigned)loaded, (unsigned)sizeof(settings));
      preferences.end();
      settings = Settings{};
      return;
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

static void disableBluetoothForPowerSave() {
  esp_bt_controller_status_t btStatus = esp_bt_controller_get_status();

  if (btStatus == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    esp_err_t err = esp_bt_controller_disable();
    if (err != ESP_OK) {
      Serial.printf("BT disable failed: %d\n", (int)err);
    }
    btStatus = esp_bt_controller_get_status();
  }

  if (btStatus == ESP_BT_CONTROLLER_STATUS_INITED) {
    esp_err_t err = esp_bt_controller_deinit();
    if (err != ESP_OK) {
      Serial.printf("BT deinit failed: %d\n", (int)err);
    }
  }

  esp_err_t rel = esp_bt_mem_release(ESP_BT_MODE_BTDM);
  if (rel != ESP_OK && rel != ESP_ERR_INVALID_STATE) {
    Serial.printf("BT mem release failed: %d\n", (int)rel);
  }
}

static void configureTimeZoneFromLocation() {
  char tzBuffer[40];
  bool inEurope = settings.latitude >= 35.0f && settings.latitude <= 72.0f &&
                  settings.longitude >= -10.0f && settings.longitude <= 40.0f;

  if (inEurope) {
    int roundedOffset = (int)lroundf(settings.longitude / 15.0f);
    if (roundedOffset < 0) {
      roundedOffset = 0;
    }
    if (roundedOffset > 3) {
      roundedOffset = 3;
    }
    snprintf(tzBuffer, sizeof(tzBuffer), "LOC%+dLOCST,M3.5.0/2,M10.5.0/3", -roundedOffset);
  } else {
    int roundedOffset = (int)lroundf(settings.longitude / 15.0f);
    if (roundedOffset > 12) {
      roundedOffset = 12;
    }
    if (roundedOffset < -12) {
      roundedOffset = -12;
    }
    snprintf(tzBuffer, sizeof(tzBuffer), "UTC%+d", -roundedOffset);
  }

  configTzTime(tzBuffer, ntpServer);
}

static float readBatteryMeasurement() {
  if (!settings.batteryPowered) {
    digitalWrite(PIN_ADC_CTRL, LOW);
    lastBatteryVoltage = NAN;
    filteredBatteryVoltage = NAN;
    batteryMeasurementSettled = false;
    return NAN;
  }

  digitalWrite(PIN_ADC_CTRL, HIGH);
  delay(4);

  uint32_t millivoltsSum = 0;
  for (uint8_t i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    millivoltsSum += (uint32_t)analogReadMilliVolts(PIN_ADC_IN);
    delay(2);
  }

  digitalWrite(PIN_ADC_CTRL, LOW);

  float averageMillivolts = (float)millivoltsSum / (float)BATTERY_ADC_SAMPLES;
  float measuredVoltage = (averageMillivolts / 1000.0f) * BATTERY_DIVIDER_RATIO;
  if (measuredVoltage < 2.0f || measuredVoltage > 5.5f) {
    return NAN;
  }

  lastBatteryVoltage = measuredVoltage;
  if (!batteryMeasurementSettled || isnan(filteredBatteryVoltage)) {
    filteredBatteryVoltage = measuredVoltage;
    batteryMeasurementSettled = true;
  } else {
    filteredBatteryVoltage += BATTERY_LOW_PASS_ALPHA * (measuredVoltage - filteredBatteryVoltage);
  }

  aprs.setBattVoltage(filteredBatteryVoltage);
  return filteredBatteryVoltage;
}

float getBatteryVoltage() {
  return filteredBatteryVoltage;
}

static void updateBatteryMeasurement() {
  if (!settings.batteryPowered) {
    if (!isnan(filteredBatteryVoltage)) {
      filteredBatteryVoltage = NAN;
      lastBatteryVoltage = NAN;
      batteryMeasurementSettled = false;
      aprs.setBattVoltage(NAN);
    }
    return;
  }

  if (lastBatteryMeasurementMs != 0 && (millis() - lastBatteryMeasurementMs) < BATTERY_MEASUREMENT_INTERVAL_MS) {
    return;
  }

  lastBatteryMeasurementMs = millis();
  readBatteryMeasurement();
  Serial.printf("Battery voltage: %.2f V\n", filteredBatteryVoltage);
}

static String formatHm(uint16_t totalMinutes) {
  char buffer[6];
  uint16_t normalized = totalMinutes % (24 * 60);
  snprintf(buffer, sizeof(buffer), "%02u:%02u", normalized / 60, normalized % 60);
  return String(buffer);
}

static bool isWithinScheduledSleepWindow(uint16_t nowMinutes, uint16_t offMinutes, uint16_t onMinutes) {
  if (offMinutes == onMinutes) {
    return false;
  }
  if (offMinutes < onMinutes) {
    return nowMinutes >= offMinutes && nowMinutes < onMinutes;
  }
  return nowMinutes >= offMinutes || nowMinutes < onMinutes;
}

static uint64_t computeSecondsUntilWake(uint16_t wakeMinutes, const struct tm& localTime, time_t now) {
  struct tm wakeTime = localTime;
  wakeTime.tm_sec = 0;
  wakeTime.tm_min = wakeMinutes % 60;
  wakeTime.tm_hour = wakeMinutes / 60;

  uint16_t nowMinutes = (uint16_t)(localTime.tm_hour * 60 + localTime.tm_min);
  if (wakeMinutes <= nowMinutes) {
    wakeTime.tm_mday += 1;
  }

  time_t wakeEpoch = mktime(&wakeTime);
  if (wakeEpoch <= now) {
    wakeEpoch = now + 60;
  }
  return (uint64_t)(wakeEpoch - now);
}

static void enterDeepSleepForSeconds(uint64_t sleepSeconds, const String& reason) {
  if (sleepSeconds == 0) {
    return;
  }

  if (settings.sendAPRS && aprs.connected()) {
    String note = reason + " " + String((unsigned long)(sleepSeconds / 60ULL)) + "min";
    if (!aprs.sendSystemStatus(note)) {
      Serial.println("APRS sleep status could not be sent");
    }
  }

  webconsole_print("Entering deep sleep: " + reason + " for " + String((unsigned long)(sleepSeconds / 60ULL)) + " min");
  delay(150);

  // Shut down LoRa radio before sleep to prevent it drawing current
  // (SX126x/LLCC68 in RX mode draws ~6 mA; sleep mode draws <2 µA)
  if (radio_phy != nullptr) {
    radio_phy->sleep();
    radio_phy = nullptr;
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
  esp_deep_sleep_start();
}

static void handleBatteryAndSleep() {
  if (!settings.batteryPowered || otaUploadInProgress) {
    return;
  }

  updateBatteryMeasurement();
  if (!isnan(filteredBatteryVoltage) && filteredBatteryVoltage < BATTERY_UNDERVOLTAGE_THRESHOLD) {
    char reason[24];
    snprintf(reason, sizeof(reason), "LOWBAT:%.2fV", filteredBatteryVoltage);
    enterDeepSleepForSeconds(LOW_BATTERY_SLEEP_SECONDS, String(reason));
    return;
  }

  if (!settings.sleepScheduleEnabled) {
    return;
  }

  if (lastBatterySleepCheckMs != 0 && (millis() - lastBatterySleepCheckMs) < 10000UL) {
    return;
  }
  lastBatterySleepCheckMs = millis();

  time_t now = time(nullptr);
  if (now < 100000) {
    return;
  }

  struct tm localTime;
  localtime_r(&now, &localTime);
  uint16_t nowMinutes = (uint16_t)(localTime.tm_hour * 60 + localTime.tm_min);
  if (!isWithinScheduledSleepWindow(nowMinutes, settings.sleepOffMinutes, settings.sleepOnMinutes)) {
    return;
  }

  uint64_t sleepSeconds = computeSecondsUntilWake(settings.sleepOnMinutes, localTime, now);
  String reason = "SLEEP:" + formatHm(settings.sleepOffMinutes) + "-" + formatHm(settings.sleepOnMinutes);
  enterDeepSleepForSeconds(sleepSeconds, reason);
}

static void handleLowBatteryAtStartup() {
  if (!settings.batteryPowered) {
    return;
  }

  float startupVoltage = readBatteryMeasurement();
  if (!isnan(startupVoltage)) {
    Serial.printf("Battery voltage (startup): %.2f V\n", startupVoltage);
  }

  if (!isnan(startupVoltage) && startupVoltage < BATTERY_UNDERVOLTAGE_THRESHOLD) {
    char reason[24];
    snprintf(reason, sizeof(reason), "LOWBAT:%.2fV", startupVoltage);
    Serial.printf("Startup low-battery guard active (%s) -> deep sleep\n", reason);
    enterDeepSleepForSeconds(LOW_BATTERY_SLEEP_SECONDS, String(reason));
  }
}

void setup() {

  // power save
    setCpuFrequencyMhz(80); // power saving, LoRa and WiFi can work fine at 40MHz


    pinMode(PIN_USERBTN,INPUT_PULLUP);
    pinMode(PIN_LED,OUTPUT);
    pinMode(PIN_ADC_CTRL, OUTPUT);
    digitalWrite(PIN_ADC_CTRL, LOW);
    pinMode(PIN_ADC_IN, INPUT);
    analogSetPinAttenuation(PIN_ADC_IN, ADC_11db);
    initSerialOutputs();
    disableBluetoothForPowerSave();
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

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

    // Initialize Improv Serial for WiFi provisioning
    //improv_begin(fw_version, "BreezedudeGS");

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
  updateBatteryMeasurement();
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
  //improv_handle();

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
