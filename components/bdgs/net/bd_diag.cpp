// Every ten minutes the station reports its state; the first report goes out
// about a minute after the start, once WiFi and the clock are there. One POST,
// 5 s timeout, no retry. Counters are cumulative since the start, the server
// forms the differences. The station list holds what was heard since the last
// report. No SSID, password, IP or other personal data goes into the report.
//
// Only these two requests carry the device key, so only they go over HTTPS,
// with the certificate checked against Let's Encrypt's roots: ISRG Root X1 and,
// as the chain leads there through a cross-signature valid until 2032, also
// ISRG Root X2. A failed HTTPS request skips the report; there is no fallback
// to HTTP. All other requests of the station stay on HTTP.
#include "bd_diag.h"

#include <ArduinoJson.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "Arduino.h"
#include "bd_console.h"
#include "bd_net.h"
#include "bd_power.h"
#include "bd_radio.h"
#include "bd_state.h"
#include "bd_web.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "helper.h"
#include "nvs.h"
#include "ota_gs.h"

static const char* TAG = "diag";

static constexpr uint32_t kIntervalS = 600;
static constexpr uint32_t kFirstReportMs = 60 * 1000;
static constexpr uint32_t kRssiSampleMs = 60 * 1000;
static constexpr int kTimeoutMs = 5000;
static constexpr int kMaxStations = 64;
static const char* kNvsNamespace = "gs_diag";

extern const char isrg_roots_pem[] asm("_binary_isrg_roots_pem_start");

struct StationHeard {
  uint8_t vid;
  uint16_t addr;
  uint16_t packets;
  float rssiSum;
  float snrSum;
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static StationHeard s_stations[kMaxStations];
static int s_station_count = 0;
static uint32_t s_weather_rx = 0, s_hwinfo_rx = 0, s_tracking_rx = 0;
static uint32_t s_uploads_ok = 0, s_uploads_failed = 0, s_queue_dropped = 0;
static uint32_t s_reconnects = 0;
static bool s_connected_once = false;

static char s_id[13] = {};
static char s_key[65] = {};
static uint32_t s_next_report_ms = kFirstReportMs;
static uint32_t s_last_rssi_sample_ms = 0;
static int32_t s_rssi_sum = 0;
static int s_rssi_min = 0;
static int s_rssi_count = 0;
static bool s_body_logged = false;

static volatile int s_claim_fd = -1;
static uint32_t s_last_ts = 0;
static int s_last_code = 0;
static bool s_accepted = false;
static bool s_rejected = false;

static void to_hex(const uint8_t* data, size_t len, char* out, bool upper) {
  for (size_t i = 0; i < len; i++) snprintf(out + 2 * i, 3, upper ? "%02X" : "%02x", data[i]);
}

void bd_diag_begin() {
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  to_hex(mac, sizeof(mac), s_id, true);

  nvs_handle_t nvs;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
    ESP_LOGE(TAG, "NVS not available, no reports");
    return;
  }
  size_t len = sizeof(s_key);
  if (nvs_get_str(nvs, "key", s_key, &len) != ESP_OK || strlen(s_key) != 64) {
    // Created once, never changed: the server knows the station by it.
    uint8_t raw[32];
    esp_fill_random(raw, sizeof(raw));
    to_hex(raw, sizeof(raw), s_key, false);
    nvs_set_str(nvs, "key", s_key);
    nvs_commit(nvs);
    ESP_LOGW(TAG, "new device key created");
  }
  uint8_t accepted = 0;
  nvs_get_u8(nvs, "accepted", &accepted);
  s_accepted = accepted;
  nvs_close(nvs);
}

String bd_diag_station_id() { return String(s_id); }

void bd_diag_note_packet(uint8_t vid, uint16_t addr, uint8_t type, float rssi, float snr) {
  portENTER_CRITICAL(&s_mux);
  if (type == FANET_PCK_TYPE_WEATHER) s_weather_rx++;
  if (type == FANET_PCK_TYPE_HW_INFO) s_hwinfo_rx++;
  if (type == FANET_PCK_TYPE_TRACKING || type == FANET_PCK_TYPE_GROUND_TRACKING) s_tracking_rx++;
  StationHeard* s = nullptr;
  for (int i = 0; i < s_station_count && !s; i++) {
    if (s_stations[i].vid == vid && s_stations[i].addr == addr) s = &s_stations[i];
  }
  if (!s && s_station_count < kMaxStations) {
    s = &s_stations[s_station_count++];
    *s = {vid, addr, 0, 0, 0};
  }
  if (s) {
    s->packets++;
    s->rssiSum += rssi;
    s->snrSum += snr;
  }
  portEXIT_CRITICAL(&s_mux);
}

void bd_diag_note_upload(bool ok) {
  portENTER_CRITICAL(&s_mux);
  (ok ? s_uploads_ok : s_uploads_failed)++;
  portEXIT_CRITICAL(&s_mux);
}

void bd_diag_note_queue_dropped() {
  portENTER_CRITICAL(&s_mux);
  s_queue_dropped++;
  portEXIT_CRITICAL(&s_mux);
}

void bd_diag_note_wifi_reconnect() {
  portENTER_CRITICAL(&s_mux);
  if (s_connected_once) s_reconnects++;
  s_connected_once = true;
  portEXIT_CRITICAL(&s_mux);
}

void bd_diag_request_claim(int sockfd) { s_claim_fd = sockfd; }

BdDiagState bd_diag_state() {
  return {!settings.disableDiagnostics, s_last_ts, s_last_code, s_accepted, s_rejected};
}

static const char* radio_name() {
  switch (lora_module) {
    case RADIO_MODULE_SX1276: return "SX1276";
    case RADIO_MODULE_LLCC68: return "LLCC68";
    case RADIO_MODULE_SX1262: return "SX1262";
    default: return nullptr;
  }
}

static const char* reset_reason_id() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "unknown";
  }
}

// __DATE__ is "Nov  3 2026".
static String build_date_iso() {
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* date = __DATE__;
  char mon[4] = {date[0], date[1], date[2], 0};
  const char* at = strstr(months, mon);
  const int month = at ? (int)(at - months) / 3 + 1 : 0;
  char out[24];
  snprintf(out, sizeof(out), "%.4s-%02d-%02d", date + 7, month % 100, atoi(date + 4) % 100);
  return String(out);
}

static String build_report() {
  JsonDocument doc;
  settings_lock();
  doc["name"] = settings.deviceName;
  doc["branch"] = settings.updateBranch;
  doc["location"]["lat"] = settings.latitude;
  doc["location"]["lon"] = settings.longitude;
  doc["location"]["alt"] = settings.elevation;
  const bool battery = settings.batteryPowered;
  JsonObject features = doc["features"].to<JsonObject>();
  features["fanet"] = settings.receiveFanet;
  features["flarm"] = settings.receiveFlarm;
  features["adsl"] = settings.receiveAdsl;
  features["aprs"] = settings.sendAPRS;
  features["breezedude"] = settings.sendBreezedude;
  features["lora_ota"] = (bool)BREEZEDUDE_GS_ENABLE_LORA_OTA;
  features["sleep_schedule"] = settings.sleepScheduleEnabled;
  settings_unlock();

  doc["fw"] = fw_version;
  doc["fw_build"] = build_date_iso();
  doc["board"] = CONFIG_BDGS_BOARD_NAME;
  doc["radio"] = radio_name();
  doc["uptime_s"] = (uint32_t)(millis() / 1000);
  doc["reset_reason"] = reset_reason_id();
  doc["interval_s"] = kIntervalS;

  JsonObject net = doc["net"].to<JsonObject>();
  net["type"] = "wifi";
  if (s_rssi_count) {
    net["rssi_avg"] = (int)lroundf((float)s_rssi_sum / s_rssi_count);
    net["rssi_min"] = s_rssi_min;
  } else {
    net["rssi_avg"] = nullptr;
    net["rssi_min"] = nullptr;
  }
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) net["channel"] = ap.primary;
  net["reconnects"] = s_reconnects;

  doc["heap"]["free"] = (uint32_t)esp_get_free_heap_size();
  doc["heap"]["min_free"] = (uint32_t)esp_get_minimum_free_heap_size();

  doc["battery"]["powered"] = battery;
  const float volts = battery ? getBatteryVoltage() : NAN;
  if (!isnan(volts)) {
    doc["battery"]["mv"] = (int)lroundf(volts * 1000.0f);
  } else {
    doc["battery"]["mv"] = nullptr;
  }
  doc["battery"]["perc"] = nullptr;

  JsonObject counters = doc["counters"].to<JsonObject>();
  JsonArray stations = doc["stations"].to<JsonArray>();
  portENTER_CRITICAL(&s_mux);
  const uint32_t weather = s_weather_rx, hwinfo = s_hwinfo_rx, tracking = s_tracking_rx;
  const uint32_t ok = s_uploads_ok, failed = s_uploads_failed, dropped = s_queue_dropped;
  const int count = s_station_count;
  StationHeard heard[kMaxStations];
  memcpy(heard, s_stations, sizeof(StationHeard) * count);
  portEXIT_CRITICAL(&s_mux);
  counters["fanet_rx"] = fanet_rx_count;
  counters["weather_rx"] = weather;
  counters["hwinfo_rx"] = hwinfo;
  counters["tracking_rx"] = tracking;
  counters["flarm_rx"] = flarm_decoded;
  counters["adsl_rx"] = adsl_decoded;
  counters["uploads_ok"] = ok;
  counters["uploads_failed"] = failed;
  counters["queue_dropped"] = dropped;
  for (int i = 0; i < count; i++) {
    JsonObject s = stations.add<JsonObject>();
    char id[7];
    snprintf(id, sizeof(id), "%02X%04X", heard[i].vid, heard[i].addr);
    s["id"] = id;
    s["packets"] = heard[i].packets;
    s["rssi"] = (int)lroundf(heard[i].rssiSum / heard[i].packets);
    s["snr"] = roundf(heard[i].snrSum / heard[i].packets * 10.0f) / 10.0f;
  }

  String out;
  serializeJson(doc, out);
  return out;
}

// One POST, no retry. Returns the HTTP status, negative without an answer.
static int post_json(const char* url, const String& body, char* response, size_t responseSize) {
  // The device key never goes out over plain HTTP.
  if (strncmp(url, "https://", 8) != 0) return -1;
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = kTimeoutMs;
  cfg.cert_pem = isrg_roots_pem;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) return -1;
  esp_http_client_set_header(client, "Content-Type", "application/json");
  settings_lock();
  esp_http_client_set_header(client, "ID", settings.deviceName);
  settings_unlock();
  esp_http_client_set_header(client, "X-GS-ID", s_id);
  esp_http_client_set_header(client, "X-GS-Key", s_key);

  int status = -1;
  if (esp_http_client_open(client, body.length()) == ESP_OK &&
      esp_http_client_write(client, body.c_str(), body.length()) == (int)body.length() &&
      esp_http_client_fetch_headers(client) >= 0) {
    status = esp_http_client_get_status_code(client);
    if (response && responseSize) {
      const int n = esp_http_client_read(client, response, responseSize - 1);
      response[n > 0 ? n : 0] = 0;
    }
  }
  esp_http_client_cleanup(client);
  return status;
}

static void set_accepted(bool accepted) {
  s_accepted = accepted;
  nvs_handle_t nvs;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_set_u8(nvs, "accepted", accepted ? 1 : 0);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

static void note_result(int code) {
  s_last_code = code;
  s_last_ts = (uint32_t)time(nullptr);
  if (code == 401) {
    if (!s_rejected) webconsole_print("Breezedude diagnostics: the server rejects this station's device key");
    s_rejected = true;
  } else if (code > 0) {
    s_rejected = false;
  }
  if (code == 204 && !s_accepted) set_accepted(true);
}

static void send_report() {
  const String body = build_report();
  if (!s_body_logged) {
    s_body_logged = true;
    ESP_LOGI(TAG, "report body: %s", body.c_str());
  }
  const int code = post_json(GS_DIAG_URL, body, nullptr, 0);
  ESP_LOGI(TAG, "report -> %d", code);
  note_result(code);
  portENTER_CRITICAL(&s_mux);
  s_station_count = 0;
  portEXIT_CRITICAL(&s_mux);
}

static void handle_claim(int fd) {
  JsonDocument resp;
  JsonObject claim = resp["diag_claim"].to<JsonObject>();
  if (settings.disableDiagnostics || !s_accepted) {
    claim["error"] = "The server does not know this station yet - wait for the first report.";
  } else if (!bd_net_sta_connected()) {
    claim["error"] = "No WiFi connection";
  } else {
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    char code[33];
    to_hex(raw, sizeof(raw), code, false);
    char answer[96] = {};
    const int status = post_json(GS_CLAIM_URL, String("{\"code\":\"") + code + "\"}", answer, sizeof(answer));
    if (status == 401) note_result(401);
    if (status == 201) {
      JsonDocument parsed;
      deserializeJson(parsed, answer);
      claim["url"] = String(GS_PORTAL_CLAIM_URL) + "?gs=" + s_id + "&code=" + code;
      claim["expires_in_s"] = parsed["expires_in_s"] | 900;
    } else if (status == 404) {
      // The server lost the station (deleted by an admin): the next report registers it again.
      set_accepted(false);
      claim["error"] = "The server does not know this station yet - wait for the next report.";
    } else if (status == 401) {
      claim["error"] = "The server rejects this station's device key.";
    } else {
      claim["error"] = status > 0 ? String("Server answered HTTP ") + status : String("No answer from the server");
    }
  }
  String out;
  serializeJson(resp, out);
  bd_ws_send(fd, out);
}

void bd_diag_tick() {
  if (!s_id[0] || !s_key[0]) return;
  const uint32_t now = millis();
  const bool connected = bd_net_sta_connected();

  if (connected && now - s_last_rssi_sample_ms >= kRssiSampleMs) {
    s_last_rssi_sample_ms = now;
    const int rssi = bd_net_sta_rssi();
    if (rssi < 0) {
      if (!s_rssi_count || rssi < s_rssi_min) s_rssi_min = rssi;
      s_rssi_sum += rssi;
      s_rssi_count++;
    }
  }

  const int claim_fd = s_claim_fd;
  if (claim_fd >= 0) {
    s_claim_fd = -1;
    handle_claim(claim_fd);
  }

  if ((int32_t)(now - s_next_report_ms) < 0) return;
  // Without WiFi or clock nothing is sent, and the radio's sensor updates go
  // first; the report then waits for the next net task pass.
  if (settings.disableDiagnostics) {
    s_next_report_ms = now + kIntervalS * 1000UL;
    portENTER_CRITICAL(&s_mux);
    s_station_count = 0;
    portEXIT_CRITICAL(&s_mux);
    return;
  }
  if (!connected || time(nullptr) < 1700000000 || ota_gs_busy()) return;

  s_next_report_ms = now + kIntervalS * 1000UL;
  send_report();
  s_rssi_sum = 0;
  s_rssi_count = 0;
}
