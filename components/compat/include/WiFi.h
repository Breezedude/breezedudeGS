// Thin WiFi facade. The real WiFi state machine lives in bd_net (ESP-IDF
// esp_wifi + esp_netif); this header only exposes what the ported protocol
// modules ask for: "are we online?" plus the socket client types.
#pragma once

#include "Client.h"
#include "IPAddress.h"
#include "WString.h"

typedef enum {
  WL_NO_SHIELD = 255,
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6
} wl_status_t;

class WiFiFacade {
 public:
  wl_status_t status();
  IPAddress localIP();
  int8_t RSSI();
  String SSID();
};

extern WiFiFacade WiFi;
