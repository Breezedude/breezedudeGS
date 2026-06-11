#include "power_management.h"

#include <WiFi.h>
#include "esp_wifi.h"
#include <esp_sleep.h>
#include <esp_bt.h>
#include <time.h>

#include "helper.h"

extern Settings settings;
extern Aprs aprs;
extern PhysicalLayer* radio_phy;
extern volatile bool otaUploadInProgress;
extern const char* ntpServer;
extern void webconsole_print(String in);

static constexpr uint8_t BATTERY_ADC_SAMPLES = 12;
static constexpr uint32_t BATTERY_MEASUREMENT_INTERVAL_MS = 15000UL;
static constexpr float BATTERY_LOW_PASS_ALPHA = 0.18f;
static constexpr float BATTERY_DIVIDER_RATIO = 4.9f;
static constexpr float BATTERY_UNDERVOLTAGE_THRESHOLD = 3.1f; // Volt
static constexpr uint64_t LOW_BATTERY_SLEEP_SECONDS = 12ULL * 60ULL * 60ULL; // 12 hours
static constexpr uint8_t PIN_ADC_CTRL = 37;
static constexpr uint8_t PIN_ADC_IN = 1;

static float filteredBatteryVoltage = NAN;
static float lastBatteryVoltage = NAN;
static uint32_t lastBatteryMeasurementMs = 0;
static bool batteryMeasurementSettled = false;
static uint32_t lastBatterySleepCheckMs = 0;

static void configureTimeZoneFromLocationImpl() {
  char tzBuffer[40];
  bool inEurope = settings.latitude >= 35.0f && settings.latitude <= 72.0f &&
                  settings.longitude >= -10.0f && settings.longitude <= 40.0f;

  if (inEurope) {
    int roundedOffset = (int)lroundf(settings.longitude / 15.0f);
    if (roundedOffset < 0) {
      roundedOffset = 0;
    }
    if (roundedOffset > 3) {
      roundedOffset = 3;
    }
    snprintf(tzBuffer, sizeof(tzBuffer), "LOC%+dLOCST,M3.5.0/2,M10.5.0/3", -roundedOffset);
  } else {
    int roundedOffset = (int)lroundf(settings.longitude / 15.0f);
    if (roundedOffset > 12) {
      roundedOffset = 12;
    }
    if (roundedOffset < -12) {
      roundedOffset = -12;
    }
    snprintf(tzBuffer, sizeof(tzBuffer), "UTC%+d", -roundedOffset);
  }

  configTzTime(tzBuffer, ntpServer);
}

static void disableBluetoothForPowerSaveImpl() {
  esp_bt_controller_status_t btStatus = esp_bt_controller_get_status();

  if (btStatus == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    esp_err_t err = esp_bt_controller_disable();
    if (err != ESP_OK) {
      Serial.printf("BT disable failed: %d\n", (int)err);
    }
    btStatus = esp_bt_controller_get_status();
  }

  if (btStatus == ESP_BT_CONTROLLER_STATUS_INITED) {
    esp_err_t err = esp_bt_controller_deinit();
    if (err != ESP_OK) {
      Serial.printf("BT deinit failed: %d\n", (int)err);
    }
  }

  esp_err_t rel = esp_bt_mem_release(ESP_BT_MODE_BTDM);
  if (rel != ESP_OK && rel != ESP_ERR_INVALID_STATE) {
    Serial.printf("BT mem release failed: %d\n", (int)rel);
  }
}

static float readBatteryMeasurement() {
  if (!settings.batteryPowered) {
    digitalWrite(PIN_ADC_CTRL, LOW);
    lastBatteryVoltage = NAN;
    filteredBatteryVoltage = NAN;
    batteryMeasurementSettled = false;
    return NAN;
  }

  digitalWrite(PIN_ADC_CTRL, HIGH);
  delay(4);

  uint32_t millivoltsSum = 0;
  for (uint8_t i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    millivoltsSum += (uint32_t)analogReadMilliVolts(PIN_ADC_IN);
    delay(2);
  }

  digitalWrite(PIN_ADC_CTRL, LOW);

  float averageMillivolts = (float)millivoltsSum / (float)BATTERY_ADC_SAMPLES;
  float measuredVoltage = (averageMillivolts / 1000.0f) * BATTERY_DIVIDER_RATIO;
  if (measuredVoltage < 2.0f || measuredVoltage > 5.5f) {
    return NAN;
  }

  lastBatteryVoltage = measuredVoltage;
  if (!batteryMeasurementSettled || isnan(filteredBatteryVoltage)) {
    filteredBatteryVoltage = measuredVoltage;
    batteryMeasurementSettled = true;
  } else {
    filteredBatteryVoltage += BATTERY_LOW_PASS_ALPHA * (measuredVoltage - filteredBatteryVoltage);
  }

  aprs.setBattVoltage(filteredBatteryVoltage);
  return filteredBatteryVoltage;
}

static void updateBatteryMeasurement() {
  if (!settings.batteryPowered) {
    if (!isnan(filteredBatteryVoltage)) {
      filteredBatteryVoltage = NAN;
      lastBatteryVoltage = NAN;
      batteryMeasurementSettled = false;
      aprs.setBattVoltage(NAN);
    }
    return;
  }

  if (lastBatteryMeasurementMs != 0 && (millis() - lastBatteryMeasurementMs) < BATTERY_MEASUREMENT_INTERVAL_MS) {
    return;
  }

  lastBatteryMeasurementMs = millis();
  readBatteryMeasurement();
  Serial.printf("Battery voltage: %.2f V\n", filteredBatteryVoltage);
}

static String formatHm(uint16_t totalMinutes) {
  char buffer[6];
  uint16_t normalized = totalMinutes % (24 * 60);
  snprintf(buffer, sizeof(buffer), "%02u:%02u", normalized / 60, normalized % 60);
  return String(buffer);
}

static bool isWithinScheduledSleepWindow(uint16_t nowMinutes, uint16_t offMinutes, uint16_t onMinutes) {
  if (offMinutes == onMinutes) {
    return false;
  }
  if (offMinutes < onMinutes) {
    return nowMinutes >= offMinutes && nowMinutes < onMinutes;
  }
  return nowMinutes >= offMinutes || nowMinutes < onMinutes;
}

static uint64_t computeSecondsUntilWake(uint16_t wakeMinutes, const struct tm& localTime, time_t now) {
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
  if (sleepSeconds == 0) {
    return;
  }

  if (settings.sendAPRS && aprs.connected()) {
    String note = reason + " " + String((unsigned long)(sleepSeconds / 60ULL)) + "min";
    if (!aprs.sendSystemStatus(note)) {
      Serial.println("APRS sleep status could not be sent");
    }
  }

  webconsole_print("Entering deep sleep: " + reason + " for " + String((unsigned long)(sleepSeconds / 60ULL)) + " min");
  delay(150);

  if (radio_phy != nullptr) {
    radio_phy->sleep();
    radio_phy = nullptr;
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
  esp_deep_sleep_start();
}

void initPowerManagementHardware() {
  setCpuFrequencyMhz(80); // power saving, LoRa and WiFi can work fine at 80MHz
  disableBluetoothForPowerSaveImpl();
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

void disableBluetoothForPowerSave() {
  disableBluetoothForPowerSaveImpl();
}

void configureTimeZoneFromLocation() {
  configureTimeZoneFromLocationImpl();
}

float getBatteryVoltage() {
  return filteredBatteryVoltage;
}

void handleBatteryAndSleep() {
  if (!settings.batteryPowered || otaUploadInProgress) {
    return;
  }

  updateBatteryMeasurement();
  if (!isnan(filteredBatteryVoltage) && filteredBatteryVoltage < BATTERY_UNDERVOLTAGE_THRESHOLD) {
    char reason[24];
    snprintf(reason, sizeof(reason), "LOWBAT:%.2fV", filteredBatteryVoltage);
    enterDeepSleepForSeconds(LOW_BATTERY_SLEEP_SECONDS, String(reason));
    return;
  }

  if (!settings.sleepScheduleEnabled) {
    return;
  }

  if (lastBatterySleepCheckMs != 0 && (millis() - lastBatterySleepCheckMs) < 10000UL) {
    return;
  }
  lastBatterySleepCheckMs = millis();

  time_t now = time(nullptr);
  if (now < 100000) {
    return;
  }

  struct tm localTime;
  localtime_r(&now, &localTime);
  uint16_t nowMinutes = (uint16_t)(localTime.tm_hour * 60 + localTime.tm_min);
  if (!isWithinScheduledSleepWindow(nowMinutes, settings.sleepOffMinutes, settings.sleepOnMinutes)) {
    return;
  }

  uint64_t sleepSeconds = computeSecondsUntilWake(settings.sleepOnMinutes, localTime, now);
  String reason = "SLEEP:" + formatHm(settings.sleepOffMinutes) + "-" + formatHm(settings.sleepOnMinutes);
  enterDeepSleepForSeconds(sleepSeconds, reason);
}

void handleLowBatteryAtStartup() {
  if (!settings.batteryPowered) {
    return;
  }

  float startupVoltage = readBatteryMeasurement();
  if (!isnan(startupVoltage)) {
    Serial.printf("Battery voltage (startup): %.2f V\n", startupVoltage);
  }

  if (!isnan(startupVoltage) && startupVoltage < BATTERY_UNDERVOLTAGE_THRESHOLD) {
    char reason[24];
    snprintf(reason, sizeof(reason), "LOWBAT:%.2fV", startupVoltage);
    Serial.printf("Startup low-battery guard active (%s) -> deep sleep\n", reason);
    enterDeepSleepForSeconds(LOW_BATTERY_SLEEP_SECONDS, String(reason));
  }
}
