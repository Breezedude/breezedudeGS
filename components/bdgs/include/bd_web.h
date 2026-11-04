// HTTP server and websocket API, see docs/web-api.md.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "esp_http_server.h"
#include "types.h"

// Reported in the hello message and the snapshot; a page speaking another one
// reloads itself.
#define BD_WS_PROTO 2

bool bd_web_start();
void bd_web_stop();

// --- websocket (web/bd_ws_out.cpp) ------------------------------------------
// Broadcasts to every connected client. Safe to call from any task. Live data:
// under load the oldest goes, and the clients that missed it are told to resync.
void bd_ws_broadcast(const String& text);
// Sends to one client, identified by its socket descriptor.
void bd_ws_send(int sockfd, const String& text);
// Sends this client the current status as soon as it can take it.
void bd_ws_request_status(int sockfd);
bool bd_ws_has_clients();
int bd_ws_client_count();

// Below the heap floor live packets are not even built.
bool bd_ws_heap_low();
void bd_ws_note_dropped();

struct BdWsOutStats {
  int clients;
  uint32_t dropped;
  uint32_t resyncs;
  uint32_t oversize;
  uint32_t jobsLost;
};
BdWsOutStats bd_ws_out_stats();

void bd_ws_out_begin(httpd_handle_t server);
void bd_ws_out_add_client(int sockfd);
void bd_ws_out_remove_client(int sockfd);
// Sends what is waiting now. HTTP server task only: for a handler that restarts
// the station and would otherwise never get to send its last words.
void bd_ws_out_flush();

// The status message into a buffer; returns its length, 0 if it did not fit.
size_t bd_ws_status_json(char* out, size_t cap);

// GET /api/snapshot (web/bd_snapshot.cpp).
esp_err_t bd_snapshot_handler(httpd_req_t* req);

// --- JSON packers -----------------------------------------------------------
void fillWeather(JsonObject w, weatherData* wt);
void fillTracking(JsonObject t, trackingData* td);
void fillHwInfo(JsonObject h, const hwInfoData* hi);
String packWeather(weatherData* wt);
String packTracking(trackingData* td);
String packHwInfo(const hwInfoData* hi);
String bytesToHex(const uint8_t* data, size_t len);

// --- helpers shared with the serial provisioning path -----------------------
String formatMinutesAsTime(uint16_t totalMinutes);
bool hasRequiredSetupValues(const char* deviceName, float lat, float lon);
bool isRequiredSetupComplete();
String getFormattedUptime(uint32_t uptimeSeconds);

// Handles one websocket text frame; exposed for the command dispatcher tests and
// used by the server's /ws handler.
void bd_ws_handle_command(int sockfd, const char* data, size_t len);
