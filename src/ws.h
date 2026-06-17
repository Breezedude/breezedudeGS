#pragma once

#include "types.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <esp_wifi.h>
#include <math.h>
#include "power_management.h"
#include "aprs.h"

#define HTTPTIMEOUT 1500

extern Settings settings; // your settings struct
extern void save_preferences();
extern bool forceReconnectSTA;
extern bool restartAP;
extern String fw_version;
extern bool update_available;
extern String update_available_version;
extern String getDeviceName(uint8_t vid, uint16_t fanet_id);
extern void onUpdateBranchChanged();
extern uint32_t fanet_rx_count;
extern Aprs aprs;
extern String breezedudeUrl;
extern String breezedudeHwInfoUrl;
extern weatherData weatherStore[MAX_DEVICES];
extern trackingData trackingStore[MAX_DEVICES];
extern hwInfoData hwInfoStore[HWINFO_MAX_STATIONS];
extern void webconsole_sync_client(AsyncWebSocketClient *client);
extern void webconsole_print(String in);
extern void aprsconsole_print(String in);
extern bool ota_verbose_ws;

struct ForwardQueueItem {
    String url;
    String payload;
};

static const size_t FORWARD_QUEUE_CAPACITY = 12;
static ForwardQueueItem forwardQueue[FORWARD_QUEUE_CAPACITY];
static size_t forwardQueueHead = 0;
static size_t forwardQueueCount = 0;
static uint32_t lastForwardSendMs = 0;
static const uint32_t FORWARD_SEND_MIN_INTERVAL_MS = 20;

static String internetStatusCache = "not avaliable";
static uint32_t internetStatusLastCheckMs = 0;
static uint32_t internetLastOkMs = 0;
static bool internetEverConnected = false;
static const uint32_t INTERNET_WATCHDOG_MS = 5UL * 60UL * 1000UL; // 5 min

// Queue outbound HTTP payloads so packet handling path stays responsive.
bool enqueue_forward_payload(const String& url, const String& payload) {
    if (payload.length() == 0) {
        return false;
    }

    // Drop the oldest item when queue is full to keep latest telemetry flowing.
    if (forwardQueueCount >= FORWARD_QUEUE_CAPACITY) {
        forwardQueueHead = (forwardQueueHead + 1) % FORWARD_QUEUE_CAPACITY;
        forwardQueueCount--;
    }

    size_t idx = (forwardQueueHead + forwardQueueCount) % FORWARD_QUEUE_CAPACITY;
    forwardQueue[idx].url = url;
    forwardQueue[idx].payload = payload;
    forwardQueueCount++;
    return true;
}

void process_forward_queue_tick() {
    if (forwardQueueCount == 0 || WiFi.status() != WL_CONNECTED) {
        return;
    }

    if ((millis() - lastForwardSendMs) < FORWARD_SEND_MIN_INTERVAL_MS) {
        return;
    }

    ForwardQueueItem item = forwardQueue[forwardQueueHead];
    forwardQueueHead = (forwardQueueHead + 1) % FORWARD_QUEUE_CAPACITY;
    forwardQueueCount--;
    lastForwardSendMs = millis();

    HTTPClient http;
    http.setTimeout(HTTPTIMEOUT);
    http.begin(item.url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("ID", settings.deviceName);

    int httpResponseCode = http.POST(item.payload);
    if (httpResponseCode > 0) {
        Serial.print("HTTP response code: ");
        Serial.println(httpResponseCode);
    } else {
        Serial.print("Error on sending POST: ");
        Serial.println(httpResponseCode);
    }
    http.end();
}

void updateInternetConnectionStatus() {
    if (internetStatusLastCheckMs != 0 && (millis() - internetStatusLastCheckMs) <= 15000) {
        return;
    }

    internetStatusLastCheckMs = millis();
    if (WiFi.status() != WL_CONNECTED) {
        internetStatusCache = "not avaliable";
        return;
    }

    HTTPClient http;
    http.setTimeout(1200);
    http.begin("http://fanet.breezedude.de/");
    int httpResponseCode = http.GET();
    http.end();

    if (httpResponseCode > 0) {
        internetStatusCache = "ok";
        internetLastOkMs = millis();
        internetEverConnected = true;
    } else {
        internetStatusCache = "not avaliable";
        // Watchdog: if WiFi is up but internet has been gone for >5 min after
        // having worked before, the upstream router likely lost internet without
        // dropping WiFi – reboot to recover.
        if (internetEverConnected &&
            (millis() - internetLastOkMs) > INTERNET_WATCHDOG_MS) {
            Serial.println("[watchdog] Internet lost >5min while WiFi connected – rebooting");
            delay(100);
            ESP.restart();
        }
    }
}


// ---- Async WiFi scan state machine ----
enum class ScanState { IDLE, SETTLING, SCANNING };
static ScanState _scanState = ScanState::IDLE;
static uint32_t _scanSettleStart = 0;
static bool _scanNeedsApstaRevert = false;
static AsyncWebSocket* _wsRef = nullptr;
#define SCAN_SETTLE_MS 600   // ms to wait after mode-switch before starting scan

void wifi_scan_tick() {
    if (_scanState == ScanState::IDLE || !_wsRef) return;

    if (_scanState == ScanState::SETTLING) {
        if (millis() - _scanSettleStart < SCAN_SETTLE_MS) return;  // wait for radio
        WiFi.scanDelete();
        WiFi.scanNetworks(true /*async*/, true /*show_hidden*/);
        _scanState = ScanState::SCANNING;
        Serial.println("wifi_scan: async scan started");
        return;
    }

    // SCANNING state – poll for completion
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;

    _scanState = ScanState::IDLE;

    JsonDocument resp;
    JsonArray arr = resp["wifi_scan"].to<JsonArray>();

    if (n > 0) {
        for (int16_t i = 0; i < n; i++) {
            JsonObject net = arr.add<JsonObject>();
            net["ssid"] = WiFi.SSID(i);
            net["rssi"] = WiFi.RSSI(i);
            net["open"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        }
        Serial.printf("wifi_scan: found %d networks\r\n", n);
    } else {
        Serial.printf("wifi_scan: no results (n=%d)\r\n", n);
    }

    WiFi.scanDelete();

    // Revert to AP-only if we temporarily opened STA and have no useful credentials.
    if (_scanNeedsApstaRevert && WiFi.status() != WL_CONNECTED && !forceReconnectSTA) {
        WiFi.mode(WIFI_MODE_AP);
    }
    _scanNeedsApstaRevert = false;

    String output;
    serializeJson(resp, output);
    if (_wsRef->availableForWriteAll()) {
        _wsRef->textAll(output);
    }
}
// ----------------------------------------

bool hasDefaultSettings() {
    return strcmp(settings.wifi_ssid, DEFAULT_STA_SSID) == 0 &&
                 strcmp(settings.wifi_password, DEFAULT_STA_PASSWD) == 0 &&
                 strcmp(settings.ap_ssid, "BD-Groundstation") == 0 &&
                 strcmp(settings.ap_password, "configureme") == 0 &&
                 strcmp(settings.deviceName, "MyGS") == 0 &&
                 settings.latitude == 47.0f &&
                 settings.longitude == 12.0f &&
                 settings.elevation == 400 &&
                 settings.keepAP == true;
}

bool hasRequiredSetupValues(const char* deviceName, float lat, float lon) {
    if (deviceName == nullptr) return false;
    bool nameChanged = strcmp(deviceName, "MyGS") != 0;
    bool latChanged = fabsf(lat - 47.0f) > 0.00001f;
    bool lonChanged = fabsf(lon - 12.0f) > 0.00001f;
    return nameChanged && latChanged && lonChanged;
}

bool isRequiredSetupComplete() {
    return hasRequiredSetupValues(settings.deviceName, settings.latitude, settings.longitude);
}

String formatMinutesAsTime(uint16_t totalMinutes) {
    char buffer[6];
    uint16_t normalized = totalMinutes % (24 * 60);
    snprintf(buffer, sizeof(buffer), "%02u:%02u", normalized / 60, normalized % 60);
    return String(buffer);
}

uint16_t parseTimeToMinutes(const JsonVariantConst& value, uint16_t fallbackMinutes) {
    if (value.isNull()) {
        return fallbackMinutes;
    }

    String text;
    if (value.is<const char*>()) {
        text = String(value.as<const char*>());
    } else if (value.is<String>()) {
        text = value.as<String>();
    } else {
        return fallbackMinutes;
    }

    text.trim();
    if (text.length() != 5 || text[2] != ':') {
        return fallbackMinutes;
    }

    if (!isdigit((unsigned char)text[0]) || !isdigit((unsigned char)text[1]) ||
        !isdigit((unsigned char)text[3]) || !isdigit((unsigned char)text[4])) {
        return fallbackMinutes;
    }

    int hours = (text[0] - '0') * 10 + (text[1] - '0');
    int minutes = (text[3] - '0') * 10 + (text[4] - '0');
    if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
        return fallbackMinutes;
    }

    return (uint16_t)(hours * 60 + minutes);
}



String getFormattedUptime(uint32_t uptimeSeconds) {

  unsigned int days    = uptimeSeconds / 86400;
  uptimeSeconds        = uptimeSeconds % 86400;
  byte hours           = uptimeSeconds / 3600;
  uptimeSeconds        = uptimeSeconds % 3600;
  byte minutes         = uptimeSeconds / 60;

  char buffer[25];
  if (days > 0) {
    snprintf(buffer, sizeof(buffer), "%ud %uh %um", days, hours, minutes);
  } else if (hours > 0) {
    snprintf(buffer, sizeof(buffer), "%uh %um", hours, minutes);
  } else {
    snprintf(buffer, sizeof(buffer), "%um", minutes);
  }
  return String(buffer);
}

void update_aprs_settings(){
if(settings.sendAPRS){
  aprs.begin(settings.deviceName,fw_version); 
  aprs.setAprsServer(settings.aprsServer, settings.aprsPort);
  aprs.setGPS(settings.latitude,settings.longitude,settings.elevation,0.0,0.0);
} else {
    aprs.end();
}
}

String getInternetConnectionStatus() {
    return internetStatusCache;
}

String packWeather(weatherData *wt){
    static JsonDocument resp;
    resp.clear();
    JsonArray weatherArray = resp["weather"].to<JsonArray>();
    JsonObject w = weatherArray.add<JsonObject>();

    w["vid"] = wt->vid;
    w["fanet_id"] = wt->fanet_id;
    w["name"] = wt->name;
    w["rssi"] = wt->rssi;
    w["snr"] = wt->snr;
    w["lat"] = wt->lat;
    w["lon"] = wt->lon;
    w["tLastMsg"] = wt->timestamp;

    if (wt->bTemp) w["temp"] = wt->temp;
    if (wt->bWind) {
        w["wHeading"] = wt->wHeading;
        w["wSpeed"] = wt->wSpeed;
        w["wGust"] = wt->wGust;
    }
    if (wt->bHumidity) w["humidity"] = wt->Humidity;
    if (wt->bBaro) w["baro"] = wt->Baro;
    if (wt->bStateOfCharge) w["charge"] = wt->Charge;

    String output;
    serializeJson(resp, output);
    return output;
}

String packTracking(trackingData *td) {
    static JsonDocument resp;
    resp.clear();
    JsonArray trackArray = resp["tracking"].to<JsonArray>();
    JsonObject t = trackArray.add<JsonObject>();

    t["vid"] = td->vid;
    t["name"] = td->name;
    t["fanet_id"] = td->fanet_id;
    t["rssi"] = td->rssi;
    t["snr"] = td->snr;
    t["lat"] = td->lat;
    t["lon"] = td->lon;
    t["alt"] = td->alt;
    t["hdop"] = td->hdop;
    t["acft"] = trck_acft_names[td->aircraftType];
    t["spd"] = td->speed;
    t["climb"] = td->climb;
    t["heading"] = td->heading;
    t["track"] = td->onlineTracking;
    t["tLastMsg"] = td->timestamp;
    if(td->state < trck_state_names->length()){
        t["state"] = trck_state_names[td->state];
    } else {
        t["state"] = "-";}

    if(td->state < 16){ // groundtracking, set speed and elevation "-"
        t["spd"] = "-";
        t["climb"] = "-";
        t["alt"] =  "-";
        t["heading"] = "-";
        t["acft"] = "-";
    }

    String output;
    serializeJson(resp, output);
    return output;
}

String bytesToHex(const uint8_t *data, size_t len) {
    String out;
    out.reserve(len * 2);
    char hex[3];
    for (size_t i = 0; i < len; i++) {
        snprintf(hex, sizeof(hex), "%02X", data[i]);
        out += hex;
    }
    return out;
}

String packHwInfo(const hwInfoData *hi) {
    static JsonDocument resp;
    resp.clear();
    JsonArray hwInfoArray = resp["hwinfo"].to<JsonArray>();
    JsonObject h = hwInfoArray.add<JsonObject>();

    h["vid"] = hi->vid;
    h["fanet_id"] = hi->fanet_id;
    h["name"] = getDeviceName(hi->vid, hi->fanet_id);
    h["devId"] = hi->devId;
    h["rssi"] = hi->rssi;
    h["snr"] = hi->snr;
    h["tLastMsg"] = hi->timestamp;
    h["tNewestHwInfo"] = getNewestHwInfoTimestamp(hi->vid, hi->fanet_id);

    h["subHeader"] = hi->subHeader;
    h["pingPongRequest"] = hi->pingPongRequest;
    h["hasSubtypeBuildDate"] = hi->hasSubtypeBuildDate;
    h["hasIcaoAddress"] = hi->hasIcaoAddress;
    h["hasUptime"] = hi->hasUptime;
    h["hasRxRssi"] = hi->hasRxRssi;
    h["hasExtendedHeader"] = hi->hasExtendedHeader;

    h["deviceType"] = hi->deviceType;
    h["isDevelopmentBuild"] = hi->isDevelopmentBuild;
    h["buildYear"] = hi->buildYear;
    h["buildMonth"] = hi->buildMonth;
    h["buildDay"] = hi->buildDay;

    if (hi->hasIcaoAddress) {
        h["icaoAddress"] = hi->icaoAddress;
    }
    if (hi->hasUptime) {
        h["uptimeMinutes"] = hi->uptimeMinutes;
    }
    if (hi->hasRxRssi) {
        h["rxRssiDbm"] = hi->rxRssiDbm;
        h["rxRssiSourceVendor"] = hi->rxRssiSourceVendor;
        h["rxRssiSourceAddress"] = hi->rxRssiSourceAddress;
    }
    if (hi->hasExtendedHeader) {
        h["extHeader"] = hi->extHeader;
    }

    h["rawLen"] = hi->rawLen;
    h["rawHex"] = bytesToHex(hi->rawAfterBuildDate, hi->rawLen);

    JsonArray rawArray = h["rawBytes"].to<JsonArray>();
    for (uint8_t i = 0; i < hi->rawLen; i++) {
        rawArray.add(hi->rawAfterBuildDate[i]);
    }

    String output;
    serializeJson(resp, output);
    return output;
}

void send_wd_to_frontend(weatherData *wt){
  String payload = packWeather(wt);
    enqueue_forward_payload(breezedudeUrl, payload);
}

void send_hwinfo_to_frontend(hwInfoData *hi) {
  String payload = packHwInfo(hi);
    enqueue_forward_payload(breezedudeHwInfoUrl, payload);
}

void ws_init(AsyncWebSocket* ws) {
    _wsRef = ws;
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {

  if (type == WS_EVT_CONNECT){
    Serial.printf("WS connected\r\n");
        webconsole_sync_client(client);
  }
  else if (type == WS_EVT_DISCONNECT){
    Serial.printf("WS disconnected\r\n");
    ota_verbose_ws = false;
  }
  else if (type == WS_EVT_PING){
    Serial.printf("WS ping\r\n");
  }
  else if (type == WS_EVT_PONG){
    Serial.printf("WS pong\r\n");
  }
  else if (type == WS_EVT_ERROR){
    Serial.printf("WS error\r\n");
  }

  else if (type == WS_EVT_DATA){

    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->opcode != WS_TEXT) return;

    // Do NOT write data[len]=0 – the buffer is exactly `len` bytes and
    // writing past it corrupts the heap.  ArduinoJson accepts (ptr, len).
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, (const char*)data, len);
    if (error) {
        client->text("{\"error\":\"Invalid JSON\"}");
        return;
    }

    const char* cmd = doc["cmd"];
    if (!cmd) {
        client->text("{\"error\":\"Missing command\"}");
        return;
    }

    else if (strcmp(cmd, "get_info") == 0) {
        JsonDocument resp;
        resp["version"] = fw_version;
        resp["update_available"] = update_available;
        resp["update_version"] = update_available_version;
        resp["compileTime"] = __DATE__ " " __TIME__;
        String output;
        serializeJson(resp, output);
        client->text(output);
    }

    else if (strcmp(cmd, "get_info_update") == 0) {
        JsonDocument resp;
        resp["uptime"] = getFormattedUptime((millis()) / 1000);
        resp["freeHeap"] = ESP.getFreeHeap();
        resp["sta_ip"] = WiFi.localIP().toString();
        resp["sta_rssi"] = WiFi.RSSI();
        resp["sta_status"] =  ((WiFi.status() == WL_CONNECTED) ? "connected" : "disconnected" );
        if(WiFi.getMode() == WIFI_AP_STA){resp["sta_status"] = resp["sta_status"].as<String>() + "+ AP";}
        resp["aprs_status"] = (!settings.sendAPRS) ? "disabled" : ((aprs.connected()) ? "connected" : "disconnected");
        resp["internet_connection"] = getInternetConnectionStatus();
        resp["update_available"] = update_available;
        resp["update_version"] = update_available_version;
        resp["fanet_rx"] = fanet_rx_count;
        if (settings.batteryPowered) {
            float batteryVoltage = getBatteryVoltage();
            if (!isnan(batteryVoltage)) {
                char batteryText[10];
                snprintf(batteryText, sizeof(batteryText), "%.2f V", batteryVoltage);
                resp["batteryVoltage"] = batteryText;
            }
        }
        String output;
        serializeJson(resp, output);
        client->text(output);
    }

    else if (strcmp(cmd, "get_settings") == 0) {
        JsonDocument resp;
        resp["deviceName"] = settings.deviceName;
        resp["lon"] = settings.longitude;
        resp["lat"] = settings.latitude;
        resp["elevation"] = settings.elevation;
        resp["batteryPowered"] = settings.batteryPowered;
        resp["sleepScheduleEnabled"] = settings.sleepScheduleEnabled;
        resp["sleepOffTime"] = formatMinutesAsTime(settings.sleepOffMinutes);
        resp["sleepOnTime"] = formatMinutesAsTime(settings.sleepOnMinutes);
        resp["sendAPRS"] = settings.sendAPRS;
        resp["aprsServer"] = settings.aprsServer;
        resp["aprsPort"] = settings.aprsPort;
        resp["sendBreezedude"] = settings.sendBreezedude;
        resp["autoUpdate"] = settings.autoUpdate;
        resp["updateBranch"] = settings.updateBranch;
        String output;
        serializeJson(resp, output);
        client->text(output);
    }

    else if (strcmp(cmd, "get_wifi") == 0) {
        JsonDocument resp;
        resp["ap_ssid"] = settings.ap_ssid;
        resp["ap_password"] = settings.ap_password;
        resp["sta_ssid"] = settings.wifi_ssid;
        resp["sta_password"] = settings.wifi_password;
        resp["keepAP"] = settings.keepAP;
        String output;
        serializeJson(resp, output);
        client->text(output);
    }
    else if (strcmp(cmd, "get_fanet") == 0) {
       time_t current_time =  time(nullptr);
       int time = doc["min"].as<int>();
       if (!time) time = 5;
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (current_time - weatherStore[i].timestamp < time*60) { // if valid packet within the last x minutes
                client->text(packWeather(&weatherStore[i]));
            }
            if (current_time - trackingStore[i].timestamp < time*60) { // if valid packet within the last x minutes
                client->text(packTracking(&trackingStore[i]));
            }
        }

        for (int stationIdx = 0; stationIdx < HWINFO_MAX_STATIONS; stationIdx++) {
            int hwCount = getHwInfoHistoryCount(stationIdx);
            for (int hwIdx = 0; hwIdx < hwCount; hwIdx++) {
                const hwInfoData *hi = getHwInfoHistoryEntry(stationIdx, hwIdx);
                if (hi != nullptr) {
                    client->text(packHwInfo(hi));
                }
            }
        }
    }

    else if (strcmp(cmd, "save_wifi") == 0) {
        // Use | "" fallback so missing keys never yield a nullptr that
        // would crash strcmp / strncpy.
        const char* new_sta_ssid  = doc["sta_ssid"]   | "";
        const char* new_sta_pass  = doc["sta_password"] | "";
        const char* new_ap_ssid   = doc["ap_ssid"]    | "";
        const char* new_ap_pass   = doc["ap_password"] | "";
        bool new_keepAP           = doc["keepAP"].as<bool>();

        bool staChanged = (strcmp(settings.wifi_ssid, new_sta_ssid) != 0) ||
                          (strcmp(settings.wifi_password, new_sta_pass) != 0);
        bool apChanged = (strcmp(settings.ap_ssid, new_ap_ssid) != 0) ||
                         (strcmp(settings.ap_password, new_ap_pass) != 0);

        Serial.printf("save_wifi: sta_ssid='%s' keepAP=%s staChanged=%s apChanged=%s\n",
                      new_sta_ssid,
                      new_keepAP ? "true" : "false",
                      staChanged ? "true" : "false",
                      apChanged ? "true" : "false");

        // Assign values safely
        strncpy(settings.wifi_ssid, new_sta_ssid, sizeof(settings.wifi_ssid));
        settings.wifi_ssid[sizeof(settings.wifi_ssid)-1] = '\0';

        strncpy(settings.wifi_password, new_sta_pass, sizeof(settings.wifi_password));
        settings.wifi_password[sizeof(settings.wifi_password)-1] = '\0';

        strncpy(settings.ap_ssid, new_ap_ssid, sizeof(settings.ap_ssid));
        settings.ap_ssid[sizeof(settings.ap_ssid)-1] = '\0';

        strncpy(settings.ap_password, new_ap_pass, sizeof(settings.ap_password));
        settings.ap_password[sizeof(settings.ap_password)-1] = '\0';

        settings.keepAP = new_keepAP;
        save_preferences();

        if (apChanged) {
            restartAP = true;
            client->text("{\"msg\":\"AP settings saved. Restarting AP...\"}");
        }

        if (staChanged) {
            forceReconnectSTA = true;
            client->text("{\"msg\":\"WiFi settings saved. Reconnecting STA...\"}");
        } else if (WiFi.status() == WL_CONNECTED) {
            JsonDocument resp;
            resp["msg"] = String("WiFi connected. IP: ") + WiFi.localIP().toString();
            String output;
            serializeJson(resp, output);
            client->text(output);
        } else {
            client->text("{\"msg\":\"WiFi settings saved. STA currently not connected.\"}");
        }
    }

else if (strcmp(cmd, "wifi_scan") == 0) {
        if (_scanState != ScanState::IDLE) {
            client->text("{\"msg\":\"Scan already running...\"}");
        } else {
            // AP-only mode can't scan – switch to AP+STA and let radio settle first.
            if (WiFi.getMode() == WIFI_MODE_AP) {
                WiFi.mode(WIFI_MODE_APSTA);
                _scanNeedsApstaRevert = true;
                Serial.println("wifi_scan: switched to APSTA, settling...");
            }
            _scanSettleStart = millis();
            _scanState = ScanState::SETTLING;
            client->text("{\"msg\":\"Scanning WiFi...\"}");
        }
}

else if (strcmp(cmd, "save_settings") == 0) {
    //Serial.println(json);

    const char* new_deviceName   = doc["deviceName"]   | "";
    const char* new_aprsServer   = doc["aprsServer"]   | "";
    const char* new_updateBranch = doc["updateBranch"] | "stable";
    String sleepOffRaw = doc["sleepOffTime"].isNull() ? String("") : String(doc["sleepOffTime"].as<const char*>());
    String sleepOnRaw = doc["sleepOnTime"].isNull() ? String("") : String(doc["sleepOnTime"].as<const char*>());
    bool updateBranchChanged = strcmp(settings.updateBranch, new_updateBranch) != 0;

    Serial.printf("save_settings: batt=%s sleepEnabled=%s off='%s' on='%s'\n",
                  doc["batteryPowered"].as<bool>() ? "true" : "false",
                  doc["sleepScheduleEnabled"].as<bool>() ? "true" : "false",
                  sleepOffRaw.c_str(),
                  sleepOnRaw.c_str());

    // Assign strings safely
    strncpy(settings.deviceName, new_deviceName, sizeof(settings.deviceName));
    settings.deviceName[sizeof(settings.deviceName)-1] = '\0';

    strncpy(settings.aprsServer, new_aprsServer, sizeof(settings.aprsServer));
    settings.aprsServer[sizeof(settings.aprsServer)-1] = '\0';

    // Assign primitive types
    settings.longitude     = doc["lon"].as<float>();
    settings.latitude      = doc["lat"].as<float>();
    settings.elevation     = doc["elevation"].as<int>();
    settings.batteryPowered = doc["batteryPowered"].as<bool>();
    settings.sleepScheduleEnabled = settings.batteryPowered && doc["sleepScheduleEnabled"].as<bool>();
    settings.sleepOffMinutes = parseTimeToMinutes(doc["sleepOffTime"], settings.sleepOffMinutes);
    settings.sleepOnMinutes = parseTimeToMinutes(doc["sleepOnTime"], settings.sleepOnMinutes);
    Serial.printf("save_settings parsed: off=%u (%s) on=%u (%s)\n",
                  settings.sleepOffMinutes,
                  formatMinutesAsTime(settings.sleepOffMinutes).c_str(),
                  settings.sleepOnMinutes,
                  formatMinutesAsTime(settings.sleepOnMinutes).c_str());
    bool requiredSetupComplete = hasRequiredSetupValues(settings.deviceName, settings.latitude, settings.longitude);
    bool requestedSendAprs = doc["sendAPRS"].as<bool>();
    bool requestedSendBreezedude = doc["sendBreezedude"].as<bool>();
    if (!requiredSetupComplete && (requestedSendAprs || requestedSendBreezedude)) {
        settings.sendAPRS = false;
        settings.sendBreezedude = false;
        client->text("{\"msg\":\"APRS und Send-to-Breezedude bleiben deaktiviert: Bitte Name sowie Latitude/Longitude sorgsam setzen und speichern.\"}");
    } else {
        settings.sendAPRS = requestedSendAprs;
        settings.sendBreezedude = requestedSendBreezedude;
    }
    settings.aprsPort      = doc["aprsPort"].as<int>();
    settings.autoUpdate    = doc["autoUpdate"].as<bool>();
    strncpy(settings.updateBranch, new_updateBranch, sizeof(settings.updateBranch));
    settings.updateBranch[sizeof(settings.updateBranch)-1] = '\0';
    
    save_preferences();
    client->text("{\"msg\":\"Settings saved\"}");
    update_aprs_settings();
    if (updateBranchChanged) {
        client->text("{\"msg\":\"Update branch changed. Checking for updates...\"}");
        onUpdateBranchChanged();
    }
}

    else if (strcmp(cmd, "set_ota_verbose") == 0) {
        ota_verbose_ws = doc["verbose"] | false;
        JsonDocument resp;
        resp["ota_verbose"] = ota_verbose_ws;
        String output;
        serializeJson(resp, output);
        client->text(output);
    }

    else if (strcmp(cmd, "reboot") == 0) {
        
        client->text("{\"msg\":\"rebooting...\"}");
        delay(10);
        client->text("{\"disconnect\":\"true\"}");
        delay(80);
        ESP.restart();
    }

    else {
        client->text("{\"error\":\"Unknown command\"}");
    }
}
}