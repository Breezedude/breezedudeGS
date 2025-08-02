#pragma once

#include "types.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "ogn.h"

#define HTTPTIMEOUT 1500

extern Settings settings; // your settings struct
extern void save_preferences();
extern bool forceReconnectSTA;
extern bool restartAP;
extern String fw_version;
extern uint32_t fanet_rx_count;
extern Ogn ogn;
extern String breezedudeUrl;
extern weatherData weatherStore[MAX_DEVICES];
extern trackingData trackingStore[MAX_DEVICES];

void update_ogn_settings(){
  ogn.begin(settings.deviceName,fw_version); 
  ogn.setAprsServer(settings.aprsServer, settings.aprsPort);
  ogn.setGPS(settings.latitude,settings.longitude,settings.elevation,0.0,0.0);
}

String packWeather(weatherData *wt){
    JsonDocument resp;
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
    JsonDocument resp;
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
    t["acft"] = td->aircraftType;
    t["spd"] = td->speed;
    t["climb"] = td->climb;
    t["heading"] = td->heading;
    t["track"] = td->onlineTracking;
    t["tLastMsg"] = td->timestamp;

    String output;
    serializeJson(resp, output);
    return output;
}

void send_wd_to_breezedude(weatherData *wt){
  HTTPClient http;
  http.setTimeout(HTTPTIMEOUT);
  http.begin(breezedudeUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("ID", settings.deviceName);

  JsonDocument doc;
  String payload = packWeather(wt);

  // Send POST
    int httpResponseCode = http.POST(payload);
    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.print("HTTP Response code: ");
        Serial.println(httpResponseCode);
        Serial.print("Response: ");
        Serial.println(response);
    } else {
        Serial.print("Error on sending POST: ");
        Serial.println(httpResponseCode);
    }

    http.end();
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {

  if (type == WS_EVT_CONNECT){
    Serial.printf("WS connected\r\n");
  }
  else if (type == WS_EVT_DISCONNECT){
    Serial.printf("WS disconnected\r\n");
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

    data[len] = 0;
    String json = (char*)data;
    //Serial.println(json);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);
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
        resp["compileTime"] = __DATE__ " " __TIME__;
        String output;
        serializeJson(resp, output);
        client->text(output);
    }

    else if (strcmp(cmd, "get_info_update") == 0) {
        JsonDocument resp;
        resp["uptime"] = (millis()) / 1000;
        resp["freeHeap"] = ESP.getFreeHeap();
        resp["sta_ip"] = WiFi.localIP().toString();
        resp["sta_rssi"] = WiFi.RSSI();
        resp["sta_status"] =  ((WiFi.status() == WL_CONNECTED) ? "connected" : "disconnected" );
        if(WiFi.getMode() == WIFI_AP_STA){resp["sta_status"] = resp["sta_status"].as<String>() + "+ AP";}
        resp["aprs_status"] = (ogn.connected()) ? "connected" : "disconnected";
        resp["fanet_rx"] = fanet_rx_count;
        String output;
        serializeJson(resp, output);
        client->text(output);
    }

    else if (strcmp(cmd, "get_settings") == 0) {
        JsonDocument resp;
        resp["deviceName"] = settings.deviceName;
        resp["stationName"] = settings.deviceName;
        resp["lon"] = settings.longitude;
        resp["lat"] = settings.latitude;
        resp["elevation"] = settings.elevation;
        resp["aprsServer"] = settings.aprsServer;
        resp["aprsPort"] = settings.aprsPort;
        resp["sendBreezedude"] = settings.sendBreezedude;
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
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (weatherStore[i].timestamp != 0) {
                client->text(packWeather(&weatherStore[i]));
            }
        }
    }

    else if (strcmp(cmd, "save_wifi") == 0) {
        Serial.println(json);
        if(doc["sta_ssid"].as<String>() != settings.wifi_ssid){
            forceReconnectSTA = true;
            client->text("{\"msg\":\"Connecting to WiFi...\"}");
        }
        if((settings.ap_ssid != doc["ap_ssid"].as<String>()) || (settings.ap_password = doc["ap_password"].as<String>())){
            restartAP = true;
            client->text("{\"msg\":\"Restarting AP...\"}");
        }

        settings.wifi_ssid = doc["sta_ssid"].as<String>();
        settings.wifi_password = doc["sta_password"].as<String>();
        settings.ap_ssid = doc["ap_ssid"].as<String>();
        settings.ap_password = doc["ap_password"].as<String>();
        settings.keepAP = doc["keepAP"].as<bool>();
        save_preferences();
        
    }

    else if (strcmp(cmd, "save_settings") == 0) {
        Serial.println(json);

        settings.deviceName = doc["name"].as<String>();
        settings.longitude = doc["lon"].as<float>();
        settings.latitude = doc["lat"].as<float>();
        settings.elevation = doc["elevation"].as<int>();
        settings.aprsServer = doc["aprsServer"].as<String>();
        settings.aprsPort = doc["aprsPort"].as<int>();
        settings.sendBreezedude = doc["sendBreezedude"].as<bool>();
        save_preferences();
        client->text("{\"msg\":\"Settings saved\"}");
        update_ogn_settings();
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