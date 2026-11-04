#include "bd_state.h"

#include <Preferences.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char* TAG = "state";

// --- configuration ----------------------------------------------------------
Settings settings;
static Preferences preferences;
static SemaphoreHandle_t s_settings_mutex = nullptr;
static SemaphoreHandle_t s_store_mutex = nullptr;

// --- firmware / update ------------------------------------------------------
String fw_version = CONFIG_APP_PROJECT_VER;
bool update_available = false;
String update_available_version = "";
bool force_update_check = false;

// --- connectivity -----------------------------------------------------------
bool wifiConnected = false;
bool forceReconnectSTA = true;  // attempt an STA connect right after boot
bool restartAP = false;
volatile bool otaUploadInProgress = false;
volatile bool otaUploadRejected = false;
const char* ntpServer = "pool.ntp.org";
String breezedudeUrl = "http://fanet.breezedude.de/submit_weather";
String breezedudeHwInfoUrl = "http://fanet.breezedude.de/submit_hwinfo";

// --- APRS -------------------------------------------------------------------
Aprs aprs;

// --- station data -----------------------------------------------------------
uint32_t fanet_rx_count = 0;

// --- web UI verbosity -------------------------------------------------------
bool ota_verbose_ws = false;

static SemaphoreHandle_t s_aprs_mutex = nullptr;

static void ensure_mutexes() {
  if (!s_settings_mutex) s_settings_mutex = xSemaphoreCreateRecursiveMutex();
  if (!s_store_mutex) s_store_mutex = xSemaphoreCreateRecursiveMutex();
  if (!s_aprs_mutex) s_aprs_mutex = xSemaphoreCreateRecursiveMutex();
}

void aprs_lock() {
  ensure_mutexes();
  xSemaphoreTakeRecursive(s_aprs_mutex, portMAX_DELAY);
}

void aprs_unlock() { xSemaphoreGiveRecursive(s_aprs_mutex); }

void settings_lock() {
  ensure_mutexes();
  xSemaphoreTakeRecursive(s_settings_mutex, portMAX_DELAY);
}

void settings_unlock() { xSemaphoreGiveRecursive(s_settings_mutex); }

void store_lock() {
  ensure_mutexes();
  xSemaphoreTakeRecursive(s_store_mutex, portMAX_DELAY);
}

void store_unlock() { xSemaphoreGiveRecursive(s_store_mutex); }

void load_preferences() {
  settings_lock();
  preferences.begin("gs_config", true);
  if (preferences.isKey("Settings")) {
    size_t loaded = preferences.getBytes("Settings", &settings, sizeof(settings));
    if (loaded == 0) {
      ESP_LOGW(TAG, "no stored settings found - using defaults");
    } else if (loaded != sizeof(settings)) {
      ESP_LOGW(TAG, "partial settings load (got %u, current size %u) - defaults kept for new fields",
               (unsigned)loaded, (unsigned)sizeof(settings));
    }
    settings.updateBranch[sizeof(settings.updateBranch) - 1] = '\0';
    // Also turns "alpha" back into "stable" when a regular build replaces an
    // alpha build: only alpha builds may follow the alpha channel.
    if (!update_branch_allowed(settings.updateBranch)) {
      strncpy(settings.updateBranch, "stable", sizeof(settings.updateBranch));
      settings.updateBranch[sizeof(settings.updateBranch) - 1] = '\0';
    }
  }
  // One test build kept this option under its own key; carry it over once.
  // save_preferences() removes the key afterwards.
  if (preferences.isKey("apAutoOff")) {
    settings.apAutoOff = preferences.getBool("apAutoOff", false);
  }
  preferences.end();
  settings_unlock();
}

void save_preferences() {
  settings_lock();
  preferences.begin("gs_config", false);
  size_t written = preferences.putBytes("Settings", &settings, sizeof(settings));
  if (preferences.isKey("apAutoOff")) preferences.remove("apAutoOff");
  preferences.end();
  settings_unlock();
  if (written != sizeof(settings)) {
    ESP_LOGE(TAG, "saving settings failed (%u of %u bytes)", (unsigned)written,
             (unsigned)sizeof(settings));
  }
}

// Any channel name the backend may publish: 1-7 lowercase letters or digits.
// Which channels exist is up to the manifest; only "alpha" stays reserved for
// test builds, so a regular build that replaces one falls back to stable.
bool update_branch_allowed(const char* branch) {
  if (!branch) return false;
  size_t len = strlen(branch);
  if (len == 0 || len >= sizeof(settings.updateBranch)) return false;
  for (size_t i = 0; i < len; i++) {
    char c = branch[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
  }
#if !CONFIG_BDGS_FW_ALPHA_CHANNEL
  if (strcmp(branch, "alpha") == 0) return false;
#endif
  return true;
}

void onUpdateBranchChanged() { force_update_check = true; }

void update_aprs_settings() {
  if (settings.sendAPRS) {
    aprs.begin(settings.deviceName, fw_version);
    aprs.setAprsServer(settings.aprsServer, settings.aprsPort);
    aprs.setGPS(settings.latitude, settings.longitude, settings.elevation, 0.0, 0.0);
  } else {
    aprs.end();
  }
}

String getDeviceName(uint8_t vid, uint16_t fanet_id) {
  store_lock();
  String name;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (weatherStore[i].timestamp != 0 && weatherStore[i].vid == vid &&
        weatherStore[i].fanet_id == fanet_id) {
      name = weatherStore[i].name;
      break;
    }

    if (trackingStore[i].timestamp != 0 && trackingStore[i].vid == vid &&
        trackingStore[i].fanet_id == fanet_id) {
      name = trackingStore[i].name;
      break;
    }
  }
  store_unlock();
  return name;
}

void setDeviceName(uint8_t vid, uint16_t fanet_id, String name) {
  store_lock();
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (weatherStore[i].timestamp != 0 && weatherStore[i].vid == vid &&
        weatherStore[i].fanet_id == fanet_id) {
      weatherStore[i].name = name;
      store_unlock();
      ESP_LOGI(TAG, "name for vid %02X id %04X set to %s", vid, fanet_id, name.c_str());
      return;
    }
  }
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (trackingStore[i].timestamp != 0 && trackingStore[i].vid == vid &&
        trackingStore[i].fanet_id == fanet_id) {
      trackingStore[i].name = name;
      store_unlock();
      ESP_LOGI(TAG, "name for vid %02X id %04X set to %s", vid, fanet_id, name.c_str());
      return;
    }
  }
  store_unlock();
  ESP_LOGD(TAG, "device vid %02X id %04X not known, name not stored", vid, fanet_id);
}
