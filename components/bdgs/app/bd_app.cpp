// Boot sequence and task set-up.
//
// Replaces setup()/loop(): every subsystem gets a task that blocks, so the idle
// task can drop the CPU into light sleep between LoRa packets.
//
//   radio  (prio 6)  DIO1 interrupt + FLARM/ADS-L window scheduler
//   http   (prio 5)  esp_http_server, owns the websocket sockets
//   net    (prio 4)  WiFi state machine, SNTP, outbound HTTP queue
//   aprs   (prio 3)  OGN uplink
//   dns    (prio 3)  captive portal responder
//   power  (prio 2)  battery, sleep schedule, PM locks
//   serial (prio 2)  provisioning handshake of the web installer
#include "bd_app.h"

#include <ArduinoJson.h>
#include <TimeLib.h>

#include "Arduino.h"
#include "bd_auth.h"
#include "bd_diag.h"
#include "bd_board.h"
#include "bd_console.h"
#include "bd_fwupdate.h"
#include "bd_migrate.h"
#include "bd_net.h"
#include "bd_power.h"
#include "bd_radio.h"
#include "bd_state.h"
#include "bd_stats.h"
#include "bd_storage.h"
#include "bd_web.h"
#include "config_gs.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ota_gs.h"
#include "sdkconfig.h"

static const char* TAG = "app";

// --- boot banner ------------------------------------------------------------
static void log_boot_banner() {
  const esp_app_desc_t* desc = esp_app_get_description();
  esp_chip_info_t chip = {};
  esp_chip_info(&chip);

  ESP_LOGI(TAG, "Breezedude GS %s (%s %s)", desc->version, desc->date, desc->time);
  ESP_LOGI(TAG, "chip %s rev %d, %d core(s), free heap %u", CONFIG_IDF_TARGET, chip.revision,
           chip.cores, (unsigned)ESP.getFreeHeap());

  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running) {
    ESP_LOGI(TAG, "running from %s at 0x%06x (%u KiB slot)", running->label,
             (unsigned)running->address, (unsigned)(running->size / 1024));
  }

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    ESP_LOGI(TAG, "woke from scheduled deep sleep");
  }
}

static void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS needs erasing: %s", esp_err_to_name(err));
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

// --- APRS task --------------------------------------------------------------
static void aprs_task(void*) {
  for (;;) {
    if (settings.sendAPRS && !otaUploadInProgress) {
      aprs_lock();
      aprs.run(bd_net_sta_connected());
      aprs_unlock();
      vTaskDelay(pdMS_TO_TICKS(500));
    } else {
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }
}

// --- serial provisioning ----------------------------------------------------
// Accepts: GS_CONFIG:{"ssid":"...","pass":"...","name":"...","lat":47.0,"lon":11.0,"alt":800}
// Replies: GS_CONFIG:OK, then GS_IP:<ip>        on success
//          GS_CONFIG:OK, then GS_WIFI:FAIL:<r>  on failure
//          GS_CONFIG:ERROR:<reason>             on parse error
static void handle_serial_line(const String& line) {
  if (!line.startsWith("GS_CONFIG:")) return;

  String json = line.substring(10);
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    Serial.println("GS_CONFIG:ERROR:json_parse_failed");
    return;
  }

  bool changed = false;
  bool hasNewSsid = doc["ssid"].is<const char*>();

  settings_lock();
  if (hasNewSsid) {
    strncpy(settings.wifi_ssid, doc["ssid"] | "", sizeof(settings.wifi_ssid) - 1);
    strncpy(settings.wifi_password, doc["pass"] | "", sizeof(settings.wifi_password) - 1);
    changed = true;
  }
  if (doc["name"].is<const char*>()) {
    strncpy(settings.deviceName, doc["name"] | "", sizeof(settings.deviceName) - 1);
    changed = true;
  }
  if (doc["lat"].is<float>()) {
    settings.latitude = doc["lat"].as<float>();
    changed = true;
  }
  if (doc["lon"].is<float>()) {
    settings.longitude = doc["lon"].as<float>();
    changed = true;
  }
  if (doc["alt"].is<int>()) {
    settings.elevation = doc["alt"].as<int>();
    changed = true;
  }

  // After applying all fields, check whether the station is now fully
  // configured. If so, enable data forwarding automatically so no manual
  // web-UI step is needed.
  bool hasWifi = strlen(settings.wifi_ssid) > 0;
  bool hasName = strlen(settings.deviceName) > 0 && strcmp(settings.deviceName, "MyGS") != 0;
  bool hasPos = !(fabsf(settings.latitude - 47.0f) < 0.001f &&
                  fabsf(settings.longitude - 12.0f) < 0.001f);
  if (hasWifi && hasName && hasPos) {
    settings.sendBreezedude = true;
    settings.sendAPRS = true;
    changed = true;
  }
  settings_unlock();

  if (changed) save_preferences();

  // Keep the reply free of interleaved log lines, like the Arduino build did.
  Serial.setDebugOutput(false);
  Serial.println("GS_CONFIG:OK");
  Serial.flush();
  Serial.setDebugOutput(true);

  update_aprs_settings();
  configureTimeZoneFromLocation();

  if (!hasNewSsid) return;

  String err = bd_net_connect_blocking(settings.wifi_ssid, settings.wifi_password);
  Serial.setDebugOutput(false);
  if (err.isEmpty()) {
    String ip = bd_net_sta_ip();
    Serial.println("Connected to WiFi, IP: " + ip);
    Serial.println("GS_IP:" + ip);
  } else {
    Serial.println("GS_WIFI:FAIL:" + err);
    bd_net_request_sta_reconnect();  // keep retrying in the background
  }
  Serial.flush();
  Serial.setDebugOutput(true);
}

static void serial_task(void*) {
  String buffer;

  // Incoming serial data wakes the chip from light sleep, so the slow poll below
  // still reacts immediately instead of sitting out a full interval.
  uart_set_wakeup_threshold(UART_NUM_0, 3);
  esp_sleep_enable_uart_wakeup(UART_NUM_0);

  for (;;) {
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\n') {
        String line = buffer;
        buffer.clear();
        line.trim();
        handle_serial_line(line);
      } else if (c != '\r') {
        if (buffer.length() < 512) buffer += c;
      }
    }

    // Provisioning happens right after flashing, while the boot grace period
    // keeps the station awake anyway; afterwards a slower poll is enough.
    vTaskDelay(pdMS_TO_TICKS(millis() < (uint32_t)CONFIG_BDGS_SLEEP_HOLD_AFTER_BOOT_S * 1000UL
                                 ? 100
                                 : 2000));
  }
}

// --- boot -------------------------------------------------------------------
void bd_app_start(void) {
  log_boot_banner();
  init_nvs();
  // Before anything else touches flash: a station updated from the Arduino
  // firmware is moved into this firmware's layout here (reboots in between).
  bd_migrate_layout_if_needed();
  bd_board_init();

  ESP_LOGI(TAG, "press the user button now to reset the configuration");
  delay(1000);
  if (bd_board_button_pressed()) {
    bd_board_led(true);
    save_preferences();  // writes the defaults
    ESP_LOGW(TAG, "configuration reset to defaults");
    delay(200);
    bd_board_led(false);
  } else {
    load_preferences();
  }

  bd_stats_init();
  bd_diag_begin();
  bd_power_init();
  // On a deep-sleep wake with a depleted battery this goes straight back to
  // sleep instead of bringing up radio and WiFi.
  handleLowBatteryAtStartup();

  bd_storage_mount();

  store_lock();
  for (int i = 0; i < MAX_DEVICES; i++) {
    weatherStore[i].timestamp = 0;
    trackingStore[i].timestamp = 0;
  }
  store_unlock();

  setSyncProvider(getNtpTime);
  setSyncInterval(300);
  configureTimeZoneFromLocation();

  bd_net_start();
  bool web_ok = bd_web_start();
  bool radio_ok = bd_radio_start();
  if (!radio_ok) {
    webconsole_print("No LoRa module detected - running as a WiFi-only station");
  }

  config_gs_begin();
  ota_gs_begin();
  update_aprs_settings();

  bd_power_start();

  // A press opens the on-demand AP window (auto-off mode) - the way back into a
  // sealed station whose WiFi credentials no longer work - and unlocks the web
  // interface for a browser that asked for it (forgotten password).
  bd_board_button_start([]() {
    bd_board_led_blink(300);
    bd_auth_button_pressed();
    bd_net_ap_activate("button");
  });

  xTaskCreate(aprs_task, "aprs", 6144, nullptr, 3, nullptr);
  xTaskCreate(serial_task, "serial", 5120, nullptr, 2, nullptr);

  // The image is confirmed later by the power task, after it has run stably
  // for CONFIG_BDGS_MARK_VALID_AFTER_S - a fresh update that crashes before
  // then is rolled back by the bootloader.
  (void)web_ok;

  ESP_LOGI(TAG, "sleep policy: %s", bd_power_state_text().c_str());

  // Signals the web installer that serial is ready and config can be sent.
  Serial.println("GS_READY");
}
