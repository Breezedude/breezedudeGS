#include "WiFi.h"

#include "esp_netif.h"
#include "esp_wifi.h"

WiFiFacade WiFi;

static esp_netif_t* sta_netif() { return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"); }

wl_status_t WiFiFacade::status() {
  esp_netif_t* netif = sta_netif();
  if (!netif) return WL_DISCONNECTED;

  esp_netif_ip_info_t ip = {};
  if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) return WL_DISCONNECTED;

  // An address alone is not proof the link is up after a silent AP loss, so
  // confirm the station is still associated.
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return WL_DISCONNECTED;
  return WL_CONNECTED;
}

IPAddress WiFiFacade::localIP() {
  esp_netif_ip_info_t ip = {};
  esp_netif_t* netif = sta_netif();
  if (netif) esp_netif_get_ip_info(netif, &ip);
  return IPAddress(ip.ip.addr);
}

int8_t WiFiFacade::RSSI() {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
  return ap.rssi;
}

String WiFiFacade::SSID() {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return String();
  return String((const char*)ap.ssid);
}
