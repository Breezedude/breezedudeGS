// WiFi, SNTP, captive portal DNS and the outbound HTTP forward queue.
//
// Replaces run_wifi() from the Arduino build: the state machine is driven by
// esp_wifi events plus a one-second tick, instead of being polled from loop().
#pragma once

#include <Arduino.h>

#include "types.h"

void bd_net_start();

bool bd_net_sta_connected();
String bd_net_sta_ip();
String bd_net_ap_ip();
bool bd_net_ap_active();
int8_t bd_net_sta_rssi();

void bd_net_request_sta_reconnect();
void bd_net_request_ap_restart();

// Opens the on-demand AP window (user button, enabling the auto-off mode). Only
// has an effect while apAutoOff is set; otherwise the AP follows its own rule.
void bd_net_ap_activate(const char* reason);
// Re-runs the AP/STA policy, e.g. after the WiFi settings changed.
void bd_net_reevaluate();

// Turns WiFi power save off for the duration of a large transfer (firmware or
// web UI download, upload). With modem sleep the AP buffers every frame until
// the station's next wake-up, which cut OTA throughput to ~12 KB/s.
// Reference counted; every true needs a matching false.
void bd_net_bulk_transfer(bool active);

// Blocking connect used by the serial provisioning handshake. Returns an empty
// string on success, otherwise one of "ssid_not_found", "auth_failed", "timeout".
String bd_net_connect_blocking(const char* ssid, const char* pass);

// Asynchronous scan; the result is broadcast as {"wifi_scan":[...]} when done.
bool bd_net_scan_start();
bool bd_net_scan_busy();

// --- outbound HTTP ----------------------------------------------------------
bool enqueue_forward_payload(const String& url, const String& payload);
void send_wd_to_frontend(weatherData* wt);
void send_hwinfo_to_frontend(hwInfoData* hi);

String getInternetConnectionStatus();
// HTTP code of the last Breezedude forward (0 = none yet, < 0 = transport
// error) and how long ago it was.
int bd_net_last_forward(uint32_t* ageSeconds);
