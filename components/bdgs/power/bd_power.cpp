#include "bd_power.h"

#include <time.h>

#include "bd_board.h"
#include "bd_console.h"
#include "bd_net.h"
#include "bd_state.h"
#include "bd_stats.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "helper.h"
#include "sdkconfig.h"

static const char* TAG = "power";

static constexpr uint8_t BATTERY_ADC_SAMPLES = 12;
static constexpr uint32_t BATTERY_MEASUREMENT_INTERVAL_MS = 15000UL;
static constexpr float BATTERY_LOW_PASS_ALPHA = 0.18f;
static constexpr float BATTERY_DIVIDER_RATIO = CONFIG_BDGS_BATTERY_DIVIDER_RATIO_MILLI / 1000.0f;
static constexpr float BATTERY_UNDERVOLTAGE_THRESHOLD = 3.1f;                 // Volt
static constexpr uint64_t LOW_BATTERY_SLEEP_SECONDS = 12ULL * 60ULL * 60ULL;  // 12 hours

static float s_filtered_voltage = NAN;
static uint32_t s_last_measurement_ms = 0;
static bool s_measurement_settled = false;
static uint32_t s_last_sleep_check_ms = 0;

static esp_pm_lock_handle_t s_no_sleep_lock = nullptr;
static int s_hold_count = 0;
static bool s_boot_hold_active = false;

// --- timezone ---------------------------------------------------------------
void configureTimeZoneFromLocation() {
  char tzBuffer[40];
  bool inEurope = settings.latitude >= 35.0f && settings.latitude <= 72.0f &&
                  settings.longitude >= -10.0f && settings.longitude <= 40.0f;

  if (inEurope) {
    int roundedOffset = (int)lroundf(settings.longitude / 15.0f);
    if (roundedOffset < 0) roundedOffset = 0;
    if (roundedOffset > 3) roundedOffset = 3;
    snprintf(tzBuffer, sizeof(tzBuffer), "LOC%+dLOCST,M3.5.0/2,M10.5.0/3", -roundedOffset);
  } else {
    int roundedOffset = (int)lroundf(settings.longitude / 15.0f);
    if (roundedOffset > 12) roundedOffset = 12;
    if (roundedOffset < -12) roundedOffset = -12;
    snprintf(tzBuffer, sizeof(tzBuffer), "UTC%+d", -roundedOffset);
  }

  setenv("TZ", tzBuffer, 1);
  tzset();
}

// --- battery ----------------------------------------------------------------
static float read_battery() {
  if (!settings.batteryPowered || BD_PIN_ADC_BATTERY < 0) {
    if (BD_PIN_ADC_CTRL >= 0) digitalWrite(BD_PIN_ADC_CTRL, 0);
    s_filtered_voltage = NAN;
    s_measurement_settled = false;
    return NAN;
  }

  if (BD_PIN_ADC_CTRL >= 0) {
    digitalWrite(BD_PIN_ADC_CTRL, 1);
    delay(4);
  }

  uint32_t millivoltsSum = 0;
  for (uint8_t i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    millivoltsSum += analogReadMilliVolts(BD_PIN_ADC_BATTERY);
    delay(2);
  }

  if (BD_PIN_ADC_CTRL >= 0) digitalWrite(BD_PIN_ADC_CTRL, 0);

  float averageMillivolts = (float)millivoltsSum / (float)BATTERY_ADC_SAMPLES;
  float measuredVoltage = (averageMillivolts / 1000.0f) * BATTERY_DIVIDER_RATIO;
  if (measuredVoltage < 2.0f || measuredVoltage > 5.5f) {
    return NAN;
  }

  if (!s_measurement_settled || isnan(s_filtered_voltage)) {
    s_filtered_voltage = measuredVoltage;
    s_measurement_settled = true;
  } else {
    s_filtered_voltage += BATTERY_LOW_PASS_ALPHA * (measuredVoltage - s_filtered_voltage);
  }

  aprs.setBattVoltage(s_filtered_voltage);
  return s_filtered_voltage;
}

float getBatteryVoltage() { return s_filtered_voltage; }

static void update_battery() {
  if (!settings.batteryPowered) {
    if (!isnan(s_filtered_voltage)) {
      s_filtered_voltage = NAN;
      s_measurement_settled = false;
      aprs.setBattVoltage(NAN);
    }
    return;
  }

  if (s_last_measurement_ms != 0 &&
      (millis() - s_last_measurement_ms) < BATTERY_MEASUREMENT_INTERVAL_MS) {
    return;
  }

  s_last_measurement_ms = millis();
  read_battery();
  bd_stats_battery(s_filtered_voltage);
  ESP_LOGD(TAG, "battery %.2f V", s_filtered_voltage);
}

// --- scheduled deep sleep ---------------------------------------------------
static String formatHm(uint16_t totalMinutes) {
  char buffer[6];
  uint16_t normalized = totalMinutes % (24 * 60);
  snprintf(buffer, sizeof(buffer), "%02u:%02u", normalized / 60, normalized % 60);
  return String(buffer);
}

static bool isWithinScheduledSleepWindow(uint16_t nowMinutes, uint16_t offMinutes,
                                         uint16_t onMinutes) {
  if (offMinutes == onMinutes) return false;
  if (offMinutes < onMinutes) return nowMinutes >= offMinutes && nowMinutes < onMinutes;
  return nowMinutes >= offMinutes || nowMinutes < onMinutes;
}

static uint64_t computeSecondsUntilWake(uint16_t wakeMinutes, const struct tm& localTime,
                                        time_t now) {
  struct tm wakeTime = localTime;
  wakeTime.tm_sec = 0;
  wakeTime.tm_min = wakeMinutes % 60;
  wakeTime.tm_hour = wakeMinutes / 60;

  uint16_t nowMinutes = (uint16_t)(localTime.tm_hour * 60 + localTime.tm_min);
  if (wakeMinutes <= nowMinutes) {
    wakeTime.tm_mday += 1;
  }

  time_t wakeEpoch = mktime(&wakeTime);
  if (wakeEpoch <= now) {
    wakeEpoch = now + 60;
  }
  return (uint64_t)(wakeEpoch - now);
}

static void enterDeepSleepForSeconds(uint64_t sleepSeconds, const String& reason) {
  if (sleepSeconds == 0) return;

  if (settings.sendAPRS && aprs.connected()) {
    String note = reason + " " + String((unsigned long)(sleepSeconds / 60ULL)) + "min";
    aprs_lock();
    bool sent = aprs.sendSystemStatus(note);
    aprs_unlock();
    if (!sent) ESP_LOGW(TAG, "APRS sleep status could not be sent");
  }

  webconsole_print("Entering deep sleep: " + reason + " for " +
                   String((unsigned long)(sleepSeconds / 60ULL)) + " min");
  delay(150);

  if (radio_phy != nullptr) {
    radio_phy->sleep();
    radio_phy = nullptr;
  }

  esp_ota_mark_app_valid_cancel_rollback();
  esp_wifi_stop();
  esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
  esp_deep_sleep_start();
}

static void handle_battery_and_sleep() {
  if (!settings.batteryPowered || otaUploadInProgress) return;

  update_battery();
  if (!isnan(s_filtered_voltage) && s_filtered_voltage < BATTERY_UNDERVOLTAGE_THRESHOLD) {
    char reason[24];
    snprintf(reason, sizeof(reason), "LOWBAT:%.2fV", s_filtered_voltage);
    enterDeepSleepForSeconds(LOW_BATTERY_SLEEP_SECONDS, String(reason));
    return;
  }

  if (!settings.sleepScheduleEnabled) return;

  if (s_last_sleep_check_ms != 0 && (millis() - s_last_sleep_check_ms) < 10000UL) return;
  s_last_sleep_check_ms = millis();

  time_t now = time(nullptr);
  if (now < 100000) return;

  struct tm localTime;
  localtime_r(&now, &localTime);
  uint16_t nowMinutes = (uint16_t)(localTime.tm_hour * 60 + localTime.tm_min);
  if (!isWithinScheduledSleepWindow(nowMinutes, settings.sleepOffMinutes,
                                    settings.sleepOnMinutes)) {
    return;
  }

  uint64_t sleepSeconds = computeSecondsUntilWake(settings.sleepOnMinutes, localTime, now);
  String reason = "SLEEP:" + formatHm(settings.sleepOffMinutes) + "-" +
                  formatHm(settings.sleepOnMinutes);
  enterDeepSleepForSeconds(sleepSeconds, reason);
}

void handleLowBatteryAtStartup() {
  if (!settings.batteryPowered) return;

  float startupVoltage = read_battery();
  if (!isnan(startupVoltage)) {
    ESP_LOGI(TAG, "battery at startup: %.2f V", startupVoltage);
  }

  if (!isnan(startupVoltage) && startupVoltage < BATTERY_UNDERVOLTAGE_THRESHOLD) {
    char reason[24];
    snprintf(reason, sizeof(reason), "LOWBAT:%.2fV", startupVoltage);
    ESP_LOGW(TAG, "startup low-battery guard active (%s) -> deep sleep", reason);
    enterDeepSleepForSeconds(LOW_BATTERY_SLEEP_SECONDS, String(reason));
  }
}

// --- light sleep ------------------------------------------------------------
void bd_power_hold_awake(bool hold) {
  if (!s_no_sleep_lock) return;
  if (hold) {
    if (s_hold_count++ == 0) esp_pm_lock_acquire(s_no_sleep_lock);
  } else if (s_hold_count > 0) {
    if (--s_hold_count == 0) esp_pm_lock_release(s_no_sleep_lock);
  }
}

String bd_power_state_text() {
#if CONFIG_BDGS_LIGHT_SLEEP
  if (serial_usb_host_connected()) return "awake (USB console attached)";
  // Checked before the generic hold count: the boot grace period holds the same
  // lock and would otherwise be reported as a transfer.
  if (s_boot_hold_active) return "awake (boot grace period)";
  if (s_hold_count > 0) return "awake (transfer in progress)";
  if (flarm_enabled() || adsl_enabled()) return "light sleep between FSK windows";
  return "light sleep until next packet";
#else
  return "light sleep disabled";
#endif
}

void bd_power_init() {
#if CONFIG_BDGS_LIGHT_SLEEP
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = CONFIG_BDGS_CPU_MAX_FREQ_MHZ;
  pm.min_freq_mhz = CONFIG_BDGS_CPU_MIN_FREQ_MHZ;
  pm.light_sleep_enable = true;
  esp_err_t err = esp_pm_configure(&pm);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_pm_configure failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "DFS %d-%d MHz with automatic light sleep", pm.min_freq_mhz, pm.max_freq_mhz);
  }

  if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "bdgs_hold", &s_no_sleep_lock) != ESP_OK) {
    s_no_sleep_lock = nullptr;
  }

  // Keep the station awake briefly after boot so the serial provisioning
  // handshake and the first update check are not slowed down by wake latency.
  if (CONFIG_BDGS_SLEEP_HOLD_AFTER_BOOT_S > 0) {
    s_boot_hold_active = true;
    bd_power_hold_awake(true);
  }
#else
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = CONFIG_BDGS_CPU_MAX_FREQ_MHZ;
  pm.min_freq_mhz = CONFIG_BDGS_CPU_MAX_FREQ_MHZ;
  pm.light_sleep_enable = false;
  esp_pm_configure(&pm);
#endif
}

// Smallest free stack each task has seen so far, in bytes. Logged after the
// boot phase (first update check done) and then hourly, so stack sizes can be
// set from measurements instead of guesses.
static void log_stack_headroom() {
  static const char* const kTasks[] = {"radio", "net",    "httpd", "aprs",
                                       "power", "serial", "dns",   "button"};
  char line[160];
  int len = snprintf(line, sizeof(line), "stack headroom:");
  for (const char* name : kTasks) {
    TaskHandle_t task = xTaskGetHandle(name);
    if (!task || len >= (int)sizeof(line)) continue;
    len += snprintf(line + len, sizeof(line) - len, " %s=%u", name,
                    (unsigned)uxTaskGetStackHighWaterMark(task));
  }
  ESP_LOGI(TAG, "%s", line);
}

static void power_task(void*) {
  const uint32_t boot_hold_ms = (uint32_t)CONFIG_BDGS_SLEEP_HOLD_AFTER_BOOT_S * 1000UL;
  uint32_t last_stack_log_ms = 0;
  bool confirmed = false;

  for (;;) {
    if (s_boot_hold_active && millis() > boot_hold_ms) {
      s_boot_hold_active = false;
      bd_power_hold_awake(false);
      ESP_LOGI(TAG, "boot grace period over, light sleep active");
      log_stack_headroom();
      last_stack_log_ms = millis();
    } else if (last_stack_log_ms != 0 && millis() - last_stack_log_ms > 3600UL * 1000UL) {
      log_stack_headroom();
      last_stack_log_ms = millis();
    }

    // The upload paths flip otaUploadInProgress; mirror it onto the PM lock so a
    // firmware transfer never competes with sleep latency.
    static bool uploadHeld = false;
    if (otaUploadInProgress && !uploadHeld) {
      uploadHeld = true;
      bd_power_hold_awake(true);
    } else if (!otaUploadInProgress && uploadHeld) {
      uploadHeld = false;
      bd_power_hold_awake(false);
    }

    // Rollback protection: only an image that has run this long without a
    // reset is confirmed. Until then the bootloader falls back on a crash.
    if (!confirmed && millis() > (uint32_t)CONFIG_BDGS_MARK_VALID_AFTER_S * 1000UL) {
      confirmed = true;
      esp_ota_img_states_t state;
      const esp_partition_t* running = esp_ota_get_running_partition();
      if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
          state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        webconsole_print("Firmware " + fw_version + " confirmed after a stable start");
        // The web UI update waited for this; check right away.
        force_update_check = true;
        bd_net_reevaluate();
      }
    }

#if CONFIG_BDGS_TEST_CRASH_AFTER_S > 0
    if (millis() > (uint32_t)CONFIG_BDGS_TEST_CRASH_AFTER_S * 1000UL) {
      ESP_LOGE(TAG, "TEST BUILD: deliberate crash to exercise the rollback path");
      abort();
    }
#endif

    // WiFi signal for the 24 h history; RSSI is cached by the driver, so this
    // costs no radio activity.
    static uint32_t lastWifiSampleMs = 0;
    if (bd_net_sta_connected() && millis() - lastWifiSampleMs >= 30000UL) {
      lastWifiSampleMs = millis();
      bd_stats_wifi(bd_net_sta_rssi());
    }

    handle_battery_and_sleep();
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

void bd_power_start() { xTaskCreate(power_task, "power", 5120, nullptr, 2, nullptr); }
