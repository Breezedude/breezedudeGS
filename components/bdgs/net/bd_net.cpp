#include "bd_diag.h"
#include "bd_net.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <lwip/sockets.h>

#include "bd_console.h"
#include "bd_fwupdate.h"
#include "bd_power.h"
#include "bd_state.h"
#include "bd_web.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char* TAG = "net";

// Breezedude uploads. The Arduino build used 1.5 s, which modem sleep cannot
// meet: the station only wakes every listen interval, so DNS, connect and the
// response regularly took longer and about every other upload was dropped.
#define HTTPTIMEOUT 8000
#define WIFI_RETRY_FAST_MS 10000
#define WIFI_RETRY_SLOW_MS 60000
#define SCAN_SETTLE_MS 600

static esp_netif_t* s_sta_netif = nullptr;
static esp_netif_t* s_ap_netif = nullptr;
static bool s_ap_active = false;
static volatile bool s_scan_busy = false;
static volatile bool s_scan_requested = false;
static uint32_t s_scan_started_ms = 0;
static bool s_sntp_started = false;
static uint32_t s_last_attempt_ms = 0;
static int s_connect_errors = 0;
static TaskHandle_t s_net_task = nullptr;
static volatile bool s_blocking_connect = false;
static volatile bool s_last_disconnect_auth_fail = false;
static volatile bool s_last_disconnect_no_ap = false;

// --- outbound HTTP queue ----------------------------------------------------
struct ForwardQueueItem {
  String url;
  String payload;
};

static const size_t FORWARD_QUEUE_CAPACITY = 12;
static ForwardQueueItem s_forward_queue[FORWARD_QUEUE_CAPACITY];
static size_t s_forward_head = 0;
static size_t s_forward_count = 0;
static SemaphoreHandle_t s_forward_mutex = nullptr;

static String s_internet_status = "not avaliable";
static uint32_t s_internet_last_check_ms = 0;
static uint32_t s_internet_last_ok_ms = 0;
static bool s_internet_ever_connected = false;
static const uint32_t INTERNET_WATCHDOG_MS = 5UL * 60UL * 1000UL;

bool enqueue_forward_payload(const String& url, const String& payload) {
  if (payload.length() == 0) return false;
  if (!s_forward_mutex) s_forward_mutex = xSemaphoreCreateMutex();

  xSemaphoreTake(s_forward_mutex, portMAX_DELAY);
  // Drop the oldest item when the queue is full to keep the latest telemetry
  // flowing.
  if (s_forward_count >= FORWARD_QUEUE_CAPACITY) {
    s_forward_head = (s_forward_head + 1) % FORWARD_QUEUE_CAPACITY;
    s_forward_count--;
    bd_diag_note_queue_dropped();
  }
  size_t idx = (s_forward_head + s_forward_count) % FORWARD_QUEUE_CAPACITY;
  s_forward_queue[idx].url = url;
  s_forward_queue[idx].payload = payload;
  s_forward_count++;
  xSemaphoreGive(s_forward_mutex);

  if (s_net_task) xTaskNotifyGive(s_net_task);
  return true;
}

void send_wd_to_frontend(weatherData* wt) { enqueue_forward_payload(breezedudeUrl, packWeather(wt)); }

void send_hwinfo_to_frontend(hwInfoData* hi) {
  enqueue_forward_payload(breezedudeHwInfoUrl, packHwInfo(hi));
}

static volatile int s_last_forward_code = 0;
static volatile uint32_t s_last_forward_ms = 0;

int bd_net_last_forward(uint32_t* ageSeconds) {
  if (ageSeconds) *ageSeconds = s_last_forward_ms ? (millis() - s_last_forward_ms) / 1000 : 0;
  return s_last_forward_code;
}

static bool forward_queue_pop(ForwardQueueItem* out) {
  if (!s_forward_mutex) return false;
  bool found = false;
  xSemaphoreTake(s_forward_mutex, portMAX_DELAY);
  if (s_forward_count > 0) {
    *out = s_forward_queue[s_forward_head];
    s_forward_queue[s_forward_head] = ForwardQueueItem{};
    s_forward_head = (s_forward_head + 1) % FORWARD_QUEUE_CAPACITY;
    s_forward_count--;
    found = true;
  }
  xSemaphoreGive(s_forward_mutex);
  return found;
}

static void process_forward_queue() {
  if (!bd_net_sta_connected()) return;

  ForwardQueueItem item;
  while (forward_queue_pop(&item)) {
    HTTPClient http;
    http.setTimeout(HTTPTIMEOUT);
    http.begin(item.url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("ID", settings.deviceName);

    int code = http.POST(item.payload);
    if (code <= 0) {
      ESP_LOGW(TAG, "forward POST failed: %d", code);
    } else {
      ESP_LOGD(TAG, "forward POST -> %d", code);
    }
    // Into the web console as well, but only when the state changes - a dead
    // uplink would otherwise repeat the line for every packet.
    const bool ok = code >= 200 && code < 300;
    bd_diag_note_upload(ok);
    const bool wasOk = s_last_forward_code >= 200 && s_last_forward_code < 300;
    if (!ok && (wasOk || s_last_forward_code == 0)) {
      webconsole_print(code <= 0 ? String("Breezedude upload failed: no response (") + code + ")"
                                 : String("Breezedude upload failed: HTTP ") + code);
    } else if (ok && s_last_forward_code != 0 && !wasOk) {
      webconsole_print("Breezedude upload working again");
    }
    s_last_forward_code = code;
    s_last_forward_ms = millis();
    http.end();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

String getInternetConnectionStatus() { return s_internet_status; }

static void update_internet_status() {
  // A full HTTP request: keep it rare while the uplink works, frequent only
  // while it fails so the watchdog below still reacts within its window.
  const uint32_t interval_ms = (s_internet_status == "ok")
                                   ? (uint32_t)CONFIG_BDGS_INTERNET_CHECK_S * 1000UL
                                   : 15000UL;
  if (s_internet_last_check_ms != 0 && (millis() - s_internet_last_check_ms) <= interval_ms) return;
  s_internet_last_check_ms = millis();

  if (!bd_net_sta_connected()) {
    s_internet_status = "not avaliable";
    return;
  }

  HTTPClient http;
  // Same modem-sleep latency as the uploads; a short timeout reads as
  // "no internet" and feeds the reboot watchdog below.
  http.setTimeout(5000);
  http.begin("http://fanet.breezedude.de/");
  int code = http.GET();
  http.end();

  if (code > 0) {
    s_internet_status = "ok";
    s_internet_last_ok_ms = millis();
    s_internet_ever_connected = true;
    return;
  }

  s_internet_status = "not avaliable";
  // Watchdog: if WiFi is up but the internet has been gone for >5 min after
  // having worked before, the upstream router likely lost its uplink without
  // dropping WiFi - reboot to recover.
  if (s_internet_ever_connected && (millis() - s_internet_last_ok_ms) > INTERNET_WATCHDOG_MS) {
    ESP_LOGE(TAG, "internet lost >5min while WiFi connected - rebooting");
    webconsole_print("Internet lost for 5 min while WiFi is up - rebooting");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
  }
}

// --- state ------------------------------------------------------------------
bool bd_net_sta_connected() {
  if (!s_sta_netif) return false;
  esp_netif_ip_info_t ip = {};
  return esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

String bd_net_sta_ip() {
  esp_netif_ip_info_t ip = {};
  if (s_sta_netif) esp_netif_get_ip_info(s_sta_netif, &ip);
  char buf[16];
  snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
  return String(buf);
}

String bd_net_ap_ip() {
  esp_netif_ip_info_t ip = {};
  if (s_ap_netif) esp_netif_get_ip_info(s_ap_netif, &ip);
  char buf[16];
  snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
  return String(buf);
}

bool bd_net_ap_active() { return s_ap_active; }

int8_t bd_net_sta_rssi() {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
  return ap.rssi;
}

void bd_net_request_sta_reconnect() {
  forceReconnectSTA = true;
  s_connect_errors = 0;
  if (s_net_task) xTaskNotifyGive(s_net_task);
}

void bd_net_request_ap_restart() {
  restartAP = true;
  if (s_net_task) xTaskNotifyGive(s_net_task);
}

// --- SNTP -------------------------------------------------------------------
static void start_sntp() {
  if (s_sntp_started) return;
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntpServer);
  config.start = true;
  config.server_from_dhcp = false;
  // One resync per hour is plenty: the FLARM window scheduler needs tens of
  // milliseconds of accuracy, not microseconds.
  config.sync_cb = nullptr;
  if (esp_netif_sntp_init(&config) == ESP_OK) {
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started against %s", ntpServer);
  }
}

// --- scan -------------------------------------------------------------------
// The scan runs in the net task, never in the web server's handler: there it
// cannot collide with the task's own connect attempts. A pending connect
// makes esp_wifi_scan_start() fail or aborts the scan, which is why scanning
// never worked on a station that had no WiFi yet - exactly when it is needed.
#define SCAN_TIMEOUT_MS 15000
#define SCAN_MAX_NETWORKS 20

bool bd_net_scan_busy() { return s_scan_busy || s_scan_requested; }

bool bd_net_scan_start() {
  if (bd_net_scan_busy()) return false;
  s_scan_requested = true;
  if (s_net_task) xTaskNotifyGive(s_net_task);
  return true;
}

static void scan_failed(const char* why) {
  ESP_LOGW(TAG, "scan failed: %s", why);
  bd_ws_broadcast(String("{\"wifi_scan_error\":\"") + why + "\"}");
}

static void run_scan() {
  s_scan_requested = false;
  s_scan_busy = true;
  s_scan_started_ms = millis();

  // Stop a connect attempt in progress; the retry logic resumes after the scan.
  if (!bd_net_sta_connected()) {
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
  }

  wifi_scan_config_t cfg = {};
  cfg.show_hidden = false;
  cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  cfg.scan_time.active.min = 120;
  cfg.scan_time.active.max = 300;

  esp_err_t err = esp_wifi_scan_start(&cfg, false);
  if (err == ESP_ERR_WIFI_STATE) {
    vTaskDelay(pdMS_TO_TICKS(SCAN_SETTLE_MS));
    err = esp_wifi_scan_start(&cfg, false);
  }
  if (err != ESP_OK) {
    s_scan_busy = false;
    scan_failed(esp_err_to_name(err));
  }
}

// Strongest entry per network name; the driver already sorts by signal.
static void publish_scan_results() {
  uint16_t found = 0;
  esp_wifi_scan_get_ap_num(&found);
  if (found > 40) found = 40;

  JsonDocument resp;
  JsonArray arr = resp["wifi_scan"].to<JsonArray>();

  if (found > 0) {
    wifi_ap_record_t* records = (wifi_ap_record_t*)calloc(found, sizeof(wifi_ap_record_t));
    if (records) {
      uint16_t count = found;
      if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
        for (uint16_t i = 0; i < count && arr.size() < SCAN_MAX_NETWORKS; i++) {
          const char* ssid = (const char*)records[i].ssid;
          if (!*ssid) continue;
          bool seen = false;
          for (uint16_t j = 0; j < i && !seen; j++) {
            seen = strcmp(ssid, (const char*)records[j].ssid) == 0;
          }
          if (seen) continue;
          JsonObject net = arr.add<JsonObject>();
          net["ssid"] = ssid;
          net["rssi"] = records[i].rssi;
          net["open"] = records[i].authmode == WIFI_AUTH_OPEN;
        }
      }
      free(records);
    }
  }
  esp_wifi_clear_ap_list();

  String out;
  serializeJson(resp, out);
  bd_ws_broadcast(out);
  ESP_LOGI(TAG, "scan done, %u networks", (unsigned)arr.size());
}

// --- events -----------------------------------------------------------------
static void on_wifi_event(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT) {
    switch (id) {
      case WIFI_EVENT_STA_DISCONNECTED: {
        auto* ev = (wifi_event_sta_disconnected_t*)data;
        wifiConnected = false;
        s_last_disconnect_auth_fail = ev->reason == WIFI_REASON_AUTH_FAIL ||
                                      ev->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                                      ev->reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
        s_last_disconnect_no_ap = ev->reason == WIFI_REASON_NO_AP_FOUND;
        ESP_LOGI(TAG, "STA disconnected, reason %d", ev->reason);
        if (s_net_task) xTaskNotifyGive(s_net_task);
        break;
      }
      case WIFI_EVENT_SCAN_DONE: {
        // The connect logic scans too; only our own scan is reported.
        if (!s_scan_busy) {
          esp_wifi_clear_ap_list();
          break;
        }
        auto* ev = (wifi_event_sta_scan_done_t*)data;
        if (ev->status == 0) {
          publish_scan_results();
        } else {
          esp_wifi_clear_ap_list();
          scan_failed("scan aborted");
        }
        s_scan_busy = false;
        if (s_net_task) xTaskNotifyGive(s_net_task);
        break;
      }
      case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "client joined the AP");
        break;
      default:
        break;
    }
    return;
  }

  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    wifiConnected = true;
    s_connect_errors = 0;
    bd_diag_note_wifi_reconnect();
    String ip = bd_net_sta_ip();
    ESP_LOGI(TAG, "STA got IP %s", ip.c_str());
    Serial.println("GS_IP:" + ip);

    String msg = "Connected to WiFi, IP: " + ip;
    webconsole_print(msg);

    JsonDocument doc;
    doc["msg"] = msg;
    doc["sta_status"] = "connected";
    doc["sta_ip"] = ip;
    String out;
    serializeJson(doc, out);
    bd_ws_broadcast(out);

    start_sntp();
    configureTimeZoneFromLocation();
    force_update_check = true;
    if (s_net_task) xTaskNotifyGive(s_net_task);
  }
}

// --- AP / STA configuration -------------------------------------------------
static void apply_ap_config() {
  wifi_config_t cfg = {};
  strncpy((char*)cfg.ap.ssid, settings.ap_ssid, sizeof(cfg.ap.ssid));
  cfg.ap.ssid_len = strlen(settings.ap_ssid);
  strncpy((char*)cfg.ap.password, settings.ap_password, sizeof(cfg.ap.password));
  cfg.ap.channel = 1;
  cfg.ap.max_connection = 4;
  cfg.ap.authmode = strlen(settings.ap_password) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  cfg.ap.beacon_interval = 100;
  esp_wifi_set_config(WIFI_IF_AP, &cfg);
}

static void apply_sta_config() {
  wifi_config_t cfg = {};
  strncpy((char*)cfg.sta.ssid, settings.wifi_ssid, sizeof(cfg.sta.ssid));
  strncpy((char*)cfg.sta.password, settings.wifi_password, sizeof(cfg.sta.password));
  cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
  // DTIM-based modem sleep: this is what keeps the station reachable while the
  // CPU is in light sleep.
  cfg.sta.listen_interval = CONFIG_BDGS_WIFI_LISTEN_INTERVAL;
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static void set_mode(wifi_mode_t mode) {
  wifi_mode_t current = WIFI_MODE_NULL;
  esp_wifi_get_mode(&current);
  if (current == mode) return;
  ESP_LOGI(TAG, "wifi mode -> %d", mode);
  esp_wifi_set_mode(mode);
}

// --- captive portal DNS -----------------------------------------------------
// Answers every A query with the AP address so phones pop the config page.
static void dns_task(void*) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "DNS socket failed");
    vTaskDelete(nullptr);
    return;
  }

  struct sockaddr_in bind_addr = {};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_addr.sin_port = htons(53);
  if (bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGE(TAG, "DNS bind failed: %d", errno);
    close(sock);
    vTaskDelete(nullptr);
    return;
  }

  uint8_t packet[256];
  for (;;) {
    struct sockaddr_in from = {};
    socklen_t from_len = sizeof(from);
    int len = recvfrom(sock, packet, sizeof(packet), 0, (struct sockaddr*)&from, &from_len);
    if (len < 12 + 5) continue;          // header + shortest possible question
    if (!s_ap_active) continue;          // no AP, no captive portal
    if ((packet[2] & 0x80) != 0) continue;  // responses are not ours to answer

    // Walk the single question to find where it ends.
    size_t pos = 12;
    while (pos < (size_t)len && packet[pos] != 0) {
      pos += packet[pos] + 1;
      if (pos > (size_t)len) break;
    }
    pos += 5;  // null label + qtype + qclass
    if (pos > (size_t)len || pos + 16 > sizeof(packet)) continue;

    esp_netif_ip_info_t ip = {};
    if (s_ap_netif) esp_netif_get_ip_info(s_ap_netif, &ip);
    if (ip.ip.addr == 0) continue;

    packet[2] = 0x81;  // response, recursion desired
    packet[3] = 0x80;  // recursion available, no error
    packet[6] = 0x00;
    packet[7] = 0x01;  // one answer
    packet[8] = packet[9] = packet[10] = packet[11] = 0;

    uint8_t* answer = &packet[pos];
    answer[0] = 0xC0;  // pointer to the question's name
    answer[1] = 0x0C;
    answer[2] = 0x00;
    answer[3] = 0x01;  // type A
    answer[4] = 0x00;
    answer[5] = 0x01;  // class IN
    answer[6] = answer[7] = 0x00;
    answer[8] = 0x00;
    answer[9] = 0x3C;  // TTL 60 s
    answer[10] = 0x00;
    answer[11] = 0x04;  // 4 byte address
    memcpy(&answer[12], &ip.ip.addr, 4);

    sendto(sock, packet, pos + 16, 0, (struct sockaddr*)&from, from_len);
  }
}

// --- blocking connect for serial provisioning -------------------------------
String bd_net_connect_blocking(const char* ssid, const char* pass) {
  const int MAX_ATTEMPTS = 3;
  const uint32_t TIMEOUT_MS = 10000;

  s_blocking_connect = true;
  settings_lock();
  strncpy(settings.wifi_ssid, ssid ? ssid : "", sizeof(settings.wifi_ssid) - 1);
  strncpy(settings.wifi_password, pass ? pass : "", sizeof(settings.wifi_password) - 1);
  settings_unlock();

  set_mode(WIFI_MODE_APSTA);
  apply_sta_config();

  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    ESP_LOGI(TAG, "WiFi connect attempt %d/%d to SSID %s", attempt, MAX_ATTEMPTS, ssid);
    s_last_disconnect_auth_fail = false;
    s_last_disconnect_no_ap = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_wifi_connect();

    uint32_t start = millis();
    while (millis() - start < TIMEOUT_MS) {
      if (bd_net_sta_connected()) {
        s_blocking_connect = false;
        return String();
      }
      if (s_last_disconnect_no_ap) {
        s_blocking_connect = false;
        return String("ssid_not_found");
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (s_last_disconnect_auth_fail && attempt == MAX_ATTEMPTS) {
      s_blocking_connect = false;
      return String("auth_failed");
    }
  }

  s_blocking_connect = false;
  return String("timeout");
}

// --- SoftAP policy ----------------------------------------------------------
// Three modes, picked in the web UI:
//   keepAP     AP always on (mains powered stations).
//   apAutoOff  AP only on demand: opened at boot (not on a deep-sleep wake) and
//              by the user button, closed after CONFIG_BDGS_AP_IDLE_OFF_MIN
//              minutes without a client. Losing the uplink does NOT reopen it -
//              on a solar station a dead router must not turn into a SoftAP that
//              drains the battery for days.
//   neither    AP as fallback whenever the uplink is down (Arduino behaviour).
static bool s_ap_window_open = false;
static uint32_t s_ap_last_client_ms = 0;

static int ap_client_count() {
  if (!s_ap_active) return 0;
  wifi_sta_list_t list = {};
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
  return list.num;
}

static bool ap_wanted(bool staConnected) {
  if (settings.keepAP) return true;
  if (!settings.apAutoOff) return !staConnected;

  if (!s_ap_window_open) return false;
  if (ap_client_count() > 0) {
    s_ap_last_client_ms = millis();
    return true;
  }
  const uint32_t idle_ms = (uint32_t)CONFIG_BDGS_AP_IDLE_OFF_MIN * 60000UL;
  if (millis() - s_ap_last_client_ms >= idle_ms) {
    s_ap_window_open = false;
    webconsole_print(String("AP off after ") + CONFIG_BDGS_AP_IDLE_OFF_MIN +
                     " min without clients - reboot or press the button to enable it");
    return false;
  }
  return true;
}

void bd_net_ap_activate(const char* reason) {
  s_ap_window_open = true;
  s_ap_last_client_ms = millis();
  if (settings.apAutoOff && !settings.keepAP) {
    webconsole_print(String("AP enabled (") + reason + "), turns off after " +
                     CONFIG_BDGS_AP_IDLE_OFF_MIN + " min without clients");
  }
  if (s_net_task) xTaskNotifyGive(s_net_task);
}

void bd_net_reevaluate() {
  if (s_net_task) xTaskNotifyGive(s_net_task);
}

static int s_bulk_transfers = 0;

void bd_net_bulk_transfer(bool active) {
  if (active) {
    if (s_bulk_transfers++ == 0) esp_wifi_set_ps(WIFI_PS_NONE);
  } else if (s_bulk_transfers > 0 && --s_bulk_transfers == 0) {
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
  }
}

// --- task -------------------------------------------------------------------
static void net_tick() {
  const bool connected = bd_net_sta_connected();
  const bool apWanted = ap_wanted(connected);

  if (restartAP) {
    restartAP = false;
    s_ap_active = false;
    set_mode(WIFI_MODE_STA);
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (apWanted) {
    set_mode(WIFI_MODE_APSTA);
    if (!s_ap_active) {
      apply_ap_config();
      s_ap_active = true;
      ESP_LOGI(TAG, "AP started: %s at %s", settings.ap_ssid, bd_net_ap_ip().c_str());
      webconsole_print(String("AP active: ") + settings.ap_ssid + " (" + bd_net_ap_ip() + ")");
    }
  } else if (s_ap_active) {
    s_ap_active = false;
    set_mode(WIFI_MODE_STA);
  }

  if (s_blocking_connect) return;

  if (s_scan_requested && !s_scan_busy) run_scan();
  if (s_scan_busy) {
    if (millis() - s_scan_started_ms < SCAN_TIMEOUT_MS) return;
    esp_wifi_scan_stop();
    s_scan_busy = false;
    scan_failed("timeout");
  }

  if (!connected && !otaUploadInProgress) {
    wifiConnected = false;
    if (strlen(settings.wifi_ssid) == 0) return;

    // After 3 quick retries back off to one attempt per minute, but never stop:
    // a brief router restart must not require a manual reboot of the station.
    const uint32_t interval = s_connect_errors < 3 ? WIFI_RETRY_FAST_MS : WIFI_RETRY_SLOW_MS;
    if (forceReconnectSTA || s_last_attempt_ms == 0 ||
        (millis() - s_last_attempt_ms) >= interval) {
      s_last_attempt_ms = millis();
      if (s_connect_errors < 3) s_connect_errors++;
      forceReconnectSTA = false;
      ESP_LOGI(TAG, "connecting to SSID %s", settings.wifi_ssid);
      apply_sta_config();
      esp_wifi_connect();
    }
    return;
  }

  if (connected && !otaUploadInProgress) {
    if (!wifiConnected) wifiConnected = true;
    update_internet_status();
    bd_fwupdate_check(false);
  }
}

static void net_task(void*) {
  // Give the radio and the web server a moment before the first connect attempt
  // so a failing AP cannot delay the UI coming up.
  vTaskDelay(pdMS_TO_TICKS(500));

  for (;;) {
    net_tick();
    process_forward_queue();
    bd_diag_tick();
    // Work arrives as a notification. The ceiling only paces the reconnect
    // logic, so it is short while disconnected and relaxed once online.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(bd_net_sta_connected() ? 5000 : 1000));
  }
}

void bd_net_start() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  s_sta_netif = esp_netif_create_default_wifi_sta();
  s_ap_netif = esp_netif_create_default_wifi_ap();

  esp_netif_set_hostname(s_sta_netif, settings.deviceName);

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event,
                                                      nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event,
                                                      nullptr, nullptr));

  // A deep-sleep wake is the station's own schedule, not a user asking for the
  // AP; only real boots (power-on, reset, brown-out, software restart) open the
  // on-demand window.
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
    s_ap_window_open = true;
    s_ap_last_client_ms = millis();
  }

  // Start in the mode the policy asks for, so an auto-off station woken from
  // deep sleep never even beacons.
  const bool apAtStart = settings.keepAP || !settings.apAutoOff || s_ap_window_open;
  ESP_ERROR_CHECK(esp_wifi_set_mode(apAtStart ? WIFI_MODE_APSTA : WIFI_MODE_STA));
  if (apAtStart) apply_ap_config();
  apply_sta_config();
  ESP_ERROR_CHECK(esp_wifi_start());
  s_ap_active = apAtStart;

  // Modem sleep with DTIM wake-ups: the AP buffers traffic while the station
  // sleeps, so the web UI and the APRS connection survive light sleep.
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

  // Runs the HTTP client paths: forward queue, internet check, update check,
  // manifest JSON, web UI download and the firmware OTA itself.
  xTaskCreate(net_task, "net", 8192, nullptr, 4, &s_net_task);
  xTaskCreate(dns_task, "dns", 3072, nullptr, 3, nullptr);
}
