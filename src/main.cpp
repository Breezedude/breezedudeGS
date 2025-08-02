#include <Arduino.h>
#include <RadioLib.h>
#include <Preferences.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include "FS.h"
#include <LittleFS.h>

#include <DNSServer.h>
#include <Update.h>

#include "helper.h"
#include "types.h"
#include "ws.h"
#include "ogn.h"

#define SPIFFS LittleFS

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
#define PIN_LORA_DIO0 11  // 
#define PIN_LORA_DIO1 14  // 
#define PIN_LORA_BUSY 13  // 
#define LORA_SYNCWORD 0xF1 //SX1262: 0xF4 0x14 https://blog.classycode.com/lora-sync-word-compatibility-between-sx127x-and-sx126x-460324d1787a is handled by RadioLib

#define PIN_USERBTN 0

String fw_version = "0.1";

SX1262 radio_sx1262 = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RESET, PIN_LORA_BUSY);
PhysicalLayer* radio_phy = nullptr;

volatile bool receivedFlag = false;

ICACHE_RAM_ATTR void setFlag(void) {
  receivedFlag = true;
}

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 3600;

String breezedudeUrl = "http://fanet.breezedude.de/submit_weather";
Preferences preferences;

Settings settings;

Ogn ogn;
uint32_t fanet_rx_count =0;

DNSServer dnsServer;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

extern weatherData weatherStore[MAX_DEVICES];
extern trackingData trackingStore[MAX_DEVICES];

bool wifiConnected = false;
unsigned long wifiLastAttempt = 0;
const unsigned long wifiRetryInterval = 10000; // retry every 10 sec
bool forceReconnectSTA = false;
bool restartAP = false;


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
      unpack_weatherdata(byteArr, &wd, radio_phy->getSNR(), radio_phy->getRSSI());
      //print_weatherData(&wd);
      int i = storeWeatherData(wd);
      if (millis() - weatherStore[i].last_send > 3000){ // filter forwareded duplicates
        weatherStore[i].last_send = millis()+2;
        ws.textAll(packWeather(&weatherStore[i]));
        if(wifiConnected && settings.sendBreezedude){
          send_wd_to_breezedude(&weatherStore[i]);
        }
        if(ogn.connected()){
          ogn.sendWeatherData(&wd);
        }
      }
    }

    else if(header->type == FANET_PCK_TYPE_NAME){
      char name [numBytes-3];
      memset(name,'\0', numBytes-3);
      memcpy(name, &byteArr[4], numBytes-4);
      setDeviceName(header->vendor, header->address, name);
      if(ogn.connected()){
        ogn.sendNameData(FANET2String(header->vendor, header->address),name,radio_phy->getSNR());
      }
    }
    else if(header->type == FANET_PCK_TYPE_TRACKING){
      trackingData td;
      unpack_trackingdata(byteArr, &td, radio_phy->getSNR(), radio_phy->getRSSI());
      int i = storeTrackingData(td); // sort and get position
      if (millis() - trackingStore[i].last_send > 800){ // filter forwareded duplicates
          trackingStore[i].last_send = millis();
        ws.textAll(packTracking(&trackingStore[i])); // send complete set including name
        if(ogn.connected()){
          ogn.sendTrackingData(&td);
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
  
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request){
        bool shouldReboot = !Update.hasError();
        AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", shouldReboot ? "Update Success. Rebooting..." : "Update Failed!");
        response->addHeader("Connection", "close");
        request->send(response);

        Serial.printf("content: %lu\r\n", request->contentLength());

        if (shouldReboot) {
            ws.textAll("{\"otaStatus\":\"Update complete. Rebooting...\"}");
            Serial.println("Rebooting");
            ws.textAll("{\"disconnect\":\"true\"}");
            delay(300);
            ESP.restart();
        }
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){

        size_t uploaded = index + len;
        size_t uploadSize = request->contentLength();
        int progress = ((uploaded * 100) / uploadSize)/5;
        static int last_perc = 0;

        if (!index){
            int type = 0; // U_FS, see Update.begin(...)
            if(filename == "littlefs.bin"){
              Serial.println("Updating LittleFS");
              size_t fsPartitionSize = getLittleFSPartitionSize();
              if(uploadSize > fsPartitionSize)
              type = 100; // U_SPIFFS
            }
            Serial.printf("Update Start: %s, %lu\n", filename.c_str(), uploadSize);
            if (!Update.begin(UPDATE_SIZE_UNKNOWN, type)){
                Update.printError(Serial);
            }
        }
        if (!Update.hasError()){

          if(last_perc != progress){
            last_perc = progress;
            String msg = "{\"otaStatus\":\"" + String(progress*5) + "%\"}";
            //ws.textAll(msg);
            Serial.println(msg);
          }
            if (Update.write(data, len) != len){
                Update.printError(Serial);
            }
        }
        if (final){
            if (Update.end(true)){
                Serial.printf("Update Success: %uB\n", index + len);
            } else {
                Update.printError(Serial);
            }
        }
    });
}


void run_wifi() {
  static uint32_t lastcheck = 0;
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
      // Todo: disconnect to force reconnect
    }
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
        if (forceReconnectSTA || (millis() - wifiLastAttempt >= wifiRetryInterval)) {
            wifiLastAttempt = millis();
            forceReconnectSTA = false;
            Serial.println("Attempting WiFi connection to SSID: " + String(settings.wifi_ssid));
            WiFi.begin(settings.wifi_ssid, settings.wifi_password);
        }
    } else {
        if (!wifiConnected) {
            wifiConnected = true;
            Serial.println("Connected to WiFi, IP: " + WiFi.localIP().toString());
        }
    }
  }

if(!server_started){
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    server_setup();
    server.begin();
    server_started = true;
}
    // If STA connection fails and AP fallback is needed, it is already handled above with AP+STA mode
}

void load_preferences(){
  preferences.getBytes("Settings", &settings, sizeof(settings));
}

void save_preferences(){
  preferences.putBytes("Settings", &settings, sizeof(settings));
}

void setup() {
    Serial.begin(115200);
    if (!LittleFS.begin()) {
        Serial.println("LittleFS Mount Failed");
        return;
    }

    preferences.begin("gs_config", false);
    load_preferences();

    // init buffers
    for (int i = 0; i < MAX_DEVICES; i++) {
      weatherStore[i].timestamp = 0;
      trackingStore[i].timestamp = 0;
    }

    
  if(radio_sx1262.begin(868.2, 250, 7, 5, LORA_SYNCWORD, 10, 12) == RADIOLIB_ERR_NONE){
      // NiceRF SX1262 issue https://github.com/jgromes/RadioLib/issues/689
      radio_phy = (PhysicalLayer*)&radio_sx1262;
      radio_phy->setPacketReceivedAction(setFlag);
      int state = radio_phy->startReceive();
      if (state == RADIOLIB_ERR_NONE) {
        Serial.println(F("success!"));
      } else {
        Serial.print(F("failed, code "));
        Serial.println(state);
        while (true) { delay(10); }
      }
    }  else {
        Serial.print(F("Radio not found"));
    }

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  setSyncProvider(getNtpTime);
  setSyncInterval(300);
  update_ogn_settings();
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
      send_wd_to_breezedude(&weatherStore[i]);
    }
  }
}


uint32_t lastCall = 0;
 
void loop() {
  dnsServer.processNextRequest();

  run_wifi();

  if(millis() - lastCall > 5000){
    lastCall = millis();
    Serial.println(millis());
  }

  handle_fanet();
  //handle_dummy();
  ogn.run(wifiConnected);
}