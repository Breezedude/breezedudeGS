#include "bd_fwupdate.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>

#include "bd_console.h"
#include "bd_net.h"
#include "bd_power.h"
#include "bd_state.h"
#include "bd_storage.h"
#include "bd_web.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char* TAG = "fwupdate";

#define UPDATE_CHECK_INTERVAL_MS (6UL * 3600UL * 1000UL)
#define UI_RETRY_INTERVAL_MS (5UL * 60UL * 1000UL)

struct FwManifestEntry {
  String version;
  String binUrl;
  String fsUrl;
  bool valid = false;
};

#if CONFIG_BDGS_FW_ALPHA_CHANNEL
static constexpr bool kInternalChannels = true;
#else
static constexpr bool kInternalChannels = false;
#endif

static uint32_t s_last_check_ms = 0;
static FwManifestEntry s_pending;

// Channels the last manifest offered, for the web UI's selection. Channels the
// backend marks "internal" are left out unless this is a test build.
struct FwChannel {
  char name[sizeof(settings.updateBranch)];
  char version[16];
  char description[96];
};
static constexpr int kMaxChannels = 8;
static FwChannel s_channels[kMaxChannels];
static int s_channel_count = 0;
static portMUX_TYPE s_channels_mux = portMUX_INITIALIZER_UNLOCKED;

static String channel_name() {
  // updateBranch is validated on load and save; "alpha" only survives in
  // builds with CONFIG_BDGS_FW_ALPHA_CHANNEL.
  const char* branch = update_branch_allowed(settings.updateBranch) ? settings.updateBranch : "stable";
  return String(CONFIG_BDGS_FW_CHANNEL_PREFIX) + "-" + branch;
}

static void copy_text(char* dst, size_t size, const char* src) {
  strncpy(dst, src ? src : "", size);
  dst[size - 1] = '\0';
}

static void remember_channels(JsonArray manifest) {
  const String prefix = String(CONFIG_BDGS_FW_CHANNEL_PREFIX) + "-";
  FwChannel found[kMaxChannels] = {};
  int count = 0;
  for (JsonObject entry : manifest) {
    const char* type = entry["type"] | "";
    if (strncmp(type, prefix.c_str(), prefix.length()) != 0) continue;
    const char* name = type + prefix.length();
    if (!update_branch_allowed(name)) continue;
    if ((entry["internal"] | false) && !kInternalChannels) continue;
    if (count == kMaxChannels) break;
    copy_text(found[count].name, sizeof(found[count].name), name);
    copy_text(found[count].version, sizeof(found[count].version),
              normalize_online_version(String(entry["version"] | "")).c_str());
    copy_text(found[count].description, sizeof(found[count].description), entry["description"] | "");
    count++;
  }

  bool changed;
  portENTER_CRITICAL(&s_channels_mux);
  changed = count != s_channel_count || memcmp(found, s_channels, sizeof(FwChannel) * count) != 0;
  memcpy(s_channels, found, sizeof(FwChannel) * count);
  s_channel_count = count;
  portEXIT_CRITICAL(&s_channels_mux);
  if (changed) bd_ws_broadcast(bd_fwupdate_channels_json());
}

String bd_fwupdate_channels_json() {
  FwChannel copy[kMaxChannels];
  int count;
  portENTER_CRITICAL(&s_channels_mux);
  count = s_channel_count;
  memcpy(copy, s_channels, sizeof(FwChannel) * count);
  portEXIT_CRITICAL(&s_channels_mux);

  JsonDocument doc;
  JsonArray list = doc["update_channels"].to<JsonArray>();
  for (int i = 0; i < count; i++) {
    JsonObject c = list.add<JsonObject>();
    c["name"] = copy[i].name;
    c["version"] = copy[i].version;
    c["description"] = copy[i].description;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String normalize_online_version(const String& versionRaw) {
  int firstDot = versionRaw.indexOf('.');
  int secondDot = versionRaw.indexOf('.', firstDot + 1);
  if (firstDot < 0 || secondDot < 0) {
    return versionRaw;
  }

  String patch = versionRaw.substring(secondDot + 1);
  if (patch.length() <= 1) {
    return versionRaw;
  }

  for (size_t i = 0; i < patch.length(); i++) {
    if (patch[i] < '0' || patch[i] > '9') {
      return versionRaw;
    }
  }

  // Some manifests append a 5-digit build marker to the patch (e.g. 0.6.215003).
  if (patch.length() > 5) {
    String normalizedPatch = patch.substring(0, patch.length() - 5);
    if (normalizedPatch.length() > 0) {
      return versionRaw.substring(0, secondDot + 1) + normalizedPatch;
    }
  }

  return versionRaw;
}

int fw_version_compare(const String& a, const String& b) {
  size_t posA = 0, posB = 0;
  for (int part = 0; part < 4; part++) {
    long numA = 0, numB = 0;
    if (posA < a.length()) {
      numA = strtol(a.c_str() + posA, nullptr, 10);
      int dot = a.indexOf('.', posA);
      posA = dot < 0 ? a.length() : (size_t)dot + 1;
    }
    if (posB < b.length()) {
      numB = strtol(b.c_str() + posB, nullptr, 10);
      int dot = b.indexOf('.', posB);
      posB = dot < 0 ? b.length() : (size_t)dot + 1;
    }
    if (numA != numB) return numA < numB ? -1 : 1;
    if (posA >= a.length() && posB >= b.length()) break;
  }
  return 0;
}

static bool fetch_manifest(FwManifestEntry* out) {
  HTTPClient http;
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(CONFIG_BDGS_FW_MANIFEST_URL)) {
    ESP_LOGW(TAG, "manifest begin failed");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    ESP_LOGW(TAG, "manifest HTTP %d", code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    ESP_LOGW(TAG, "manifest is not valid JSON");
    return false;
  }
  remember_channels(doc.as<JsonArray>());

  const String wanted = channel_name();
  for (JsonObject entry : doc.as<JsonArray>()) {
    const char* type = entry["type"] | "";
    if (wanted != type) continue;
    if ((entry["internal"] | false) && !kInternalChannels) break;

    const char* host = entry["host"] | "";
    int port = entry["port"] | 80;
    const char* bin = entry["bin"] | "";
    // "www" is this firmware's web UI image. Arduino stations read the same
    // channel with esp32FOTA, which only knows "bin" and "littlefs" - so the
    // entry that rolls this firmware out carries "www" and no "littlefs",
    // and the Arduino side leaves its filesystem alone.
    const char* fs = entry["www"] | "";
    if (!*fs) fs = entry["littlefs"] | "";
    if (!*host || !*bin) break;

    String base = String("http://") + host;
    if (port != 80) base += ":" + String((unsigned long)port);

    out->version = normalize_online_version(String(entry["version"] | ""));
    out->binUrl = base + bin;
    out->fsUrl = *fs ? (base + fs) : String();
    out->valid = true;
    return true;
  }

  ESP_LOGW(TAG, "no manifest entry for channel %s", wanted.c_str());
  webconsole_print("No firmware published for update channel " + wanted);
  return false;
}

// --- web UI image -----------------------------------------------------------
// The "www" partition is a single copy. A mismatching image is skipped rather
// than written: the firmware's own fallback updater page can always re-upload it.
static bool download_web_image(const String& url) {
  if (url.isEmpty()) return true;

  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, BD_WWW_PARTITION_LABEL);
  if (!part) return false;

  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  if (!http.begin(url)) return false;
  if (http.GET() != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  const int size = http.getSize();
  if (size <= 0 || (size_t)size > part->size) {
    webconsole_print(String("Web UI image does not fit this layout (") + size + " > " +
                     (unsigned)part->size + " bytes) - skipped");
    http.end();
    return false;
  }

  bd_storage_unmount();
  if (esp_partition_erase_range(part, 0, part->size) != ESP_OK) {
    http.end();
    bd_storage_mount();
    return false;
  }

  // Heap, not stack: this runs in the net task next to the HTTP client.
  constexpr size_t CHUNK = 1024;
  uint8_t* buffer = (uint8_t*)malloc(CHUNK);
  size_t written = 0;
  WiFiClient* stream = http.getStreamPtr();
  uint32_t deadline = millis() + 60000;
  while (buffer && written < (size_t)size && millis() < deadline) {
    int got = stream->read(buffer, CHUNK);
    if (got <= 0) {
      delay(5);
      continue;
    }
    if (esp_partition_write(part, written, buffer, (size_t)got) != ESP_OK) {
      break;
    }
    written += (size_t)got;
  }
  free(buffer);
  http.end();

  bool ok = written == (size_t)size;
  webconsole_print(ok ? "Web UI image updated" : "Web UI image update incomplete");
  bd_storage_mount();
  return ok;
}

static bool install_web_image(const String& url) {
  bd_net_bulk_transfer(true);
  const bool ok = download_web_image(url);
  bd_net_bulk_transfer(false);
  return ok;
}

// A freshly installed image stays "pending verification" until the power task
// confirms it. Until then it must not replace the web UI: if it is rolled back,
// the previous firmware would be left with a UI it cannot get rid of, because
// the channel only offers the newest one.
static bool running_image_confirmed() {
  esp_ota_img_states_t state;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (esp_ota_get_state_partition(running, &state) != ESP_OK) return true;  // no OTA state
  return state != ESP_OTA_IMG_PENDING_VERIFY;
}

// --- web UI follows the firmware ---------------------------------------------
// A release is firmware plus its web UI under one version. The firmware is
// always installed first; after the reboot the new firmware notices that the
// installed UI carries another version and fetches its own. Doing it in this
// order means a rolled-back firmware keeps the UI it was running with, and a
// failed UI download is simply retried.
//
// Returns false only when the matching UI is in the channel but could not be
// installed - worth a quick retry.
static bool sync_web_ui(const FwManifestEntry& entry) {
  const bool present = bd_storage_has_web_ui();
  const String installed = bd_storage_web_ui_version();
  if (present && installed == fw_version) return true;
  if (entry.fsUrl.isEmpty()) return true;  // channel offers no UI at all

  // The channel's UI belongs to the channel's firmware. Only take it if that is
  // us - or if there is no UI at all, where anything beats the fallback page.
  const bool matching = entry.version == fw_version;
  if (present && !matching) return true;

  // An unconfirmed image keeps the existing UI for now; the power task runs
  // another check right after confirming it. A missing UI is replaced at once.
  if (present && !running_image_confirmed()) {
    webconsole_print("Web UI update waits until firmware " + fw_version + " is confirmed");
    return true;
  }

  if (!present) {
    webconsole_print("Web UI missing - installing it from the update channel");
  } else {
    webconsole_print("Web UI " + (installed.isEmpty() ? String("(unversioned)") : installed) +
                     " does not match firmware " + fw_version + " - updating it");
  }

  const bool ok = install_web_image(entry.fsUrl) && bd_storage_has_web_ui();
  return ok || !matching;
}

// Version of an image that booted but never confirmed itself, so the
// bootloader rolled it back. Installing it again would repeat that forever.
static String rolled_back_version() {
  const esp_partition_t* invalid = esp_ota_get_last_invalid_partition();
  if (!invalid) return String();
  esp_app_desc_t desc = {};
  if (esp_ota_get_partition_description(invalid, &desc) != ESP_OK) return String();
  desc.version[sizeof(desc.version) - 1] = '\0';
  return String(desc.version);
}

// --- application image ------------------------------------------------------
bool bd_fwupdate_install() {
  if (!s_pending.valid) return false;

  // A fresh image counts as unconfirmed for its first minutes (rollback
  // protection), and an unconfirmed image may not start another OTA. One that
  // got far enough to download its successor is healthy enough.
  esp_ota_mark_app_valid_cancel_rollback();

  bd_power_hold_awake(true);
  bd_net_bulk_transfer(true);
  otaUploadInProgress = true;
  webconsole_print("Downloading firmware " + s_pending.version);

  esp_http_client_config_t http_cfg = {};
  http_cfg.url = s_pending.binUrl.c_str();
  http_cfg.timeout_ms = 15000;
  http_cfg.keep_alive_enable = true;
  http_cfg.buffer_size = 4096;
  // Plain HTTP needs CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP; an https:// URL needs a
  // way to verify the server, or esp_https_ota refuses to start.
  if (s_pending.binUrl.startsWith("https://")) {
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
  }

  esp_https_ota_config_t ota_cfg = {};
  ota_cfg.http_config = &http_cfg;

  esp_https_ota_handle_t handle = nullptr;
  esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
  if (err != ESP_OK || !handle) {
    webconsole_print(String("Firmware update could not start: ") + esp_err_to_name(err));
    otaUploadInProgress = false;
    bd_net_bulk_transfer(false);
    bd_power_hold_awake(false);
    return false;
  }

  int total = esp_https_ota_get_image_size(handle);
  int lastPercent = -10;
  while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    int done = esp_https_ota_get_image_len_read(handle);
    if (total > 0) {
      int percent = (done * 100) / total;
      if (percent >= lastPercent + 20) {
        lastPercent = percent;
        webconsole_print(String("Firmware download ") + percent + "%");
      }
    }
  }

  const bool complete = esp_https_ota_is_complete_data_received(handle);
  esp_err_t finish = esp_https_ota_finish(handle);

  if (err != ESP_OK || !complete || (finish != ESP_OK && finish != ESP_ERR_NOT_FINISHED)) {
    webconsole_print(String("Firmware update failed: ") + esp_err_to_name(err != ESP_OK ? err : finish));
    otaUploadInProgress = false;
    bd_net_bulk_transfer(false);
    bd_power_hold_awake(false);
    return false;
  }

  // The matching web UI is fetched by the new firmware after the reboot.
  webconsole_print("Firmware " + s_pending.version + " installed - rebooting");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return true;  // not reached
}

void bd_fwupdate_check(bool force) {
  if (!bd_net_sta_connected()) return;
  if (!force && !force_update_check && s_last_check_ms != 0 &&
      (millis() - s_last_check_ms) < UPDATE_CHECK_INTERVAL_MS) {
    return;
  }
  force_update_check = false;
  s_last_check_ms = millis();

  webconsole_print("Checking GS firmware updates (channel: " + channel_name() + ")");

  FwManifestEntry entry;
  if (!fetch_manifest(&entry)) return;

  s_pending = entry;
  update_available_version = entry.version;
  update_available = fw_version_compare(fw_version, entry.version) < 0;

  // Covers the first boot after a firmware update or a layout migration, and
  // repairs a UI that went missing. A failed download is retried in minutes
  // rather than at the next six-hourly check.
  if (!sync_web_ui(entry)) {
    s_last_check_ms = millis() - UPDATE_CHECK_INTERVAL_MS + UI_RETRY_INTERVAL_MS;
  }

  if (!update_available) {
    webconsole_print("GS firmware up to date: " + fw_version + " (online " + entry.version + ")");
    return;
  }

  if (entry.version == rolled_back_version()) {
    webconsole_print("Update " + entry.version +
                     " was rolled back on this station before - not installing it again");
    return;
  }

  if (settings.autoUpdate) {
    webconsole_print("Update available: " + entry.version + " (current " + fw_version +
                     "). Starting update...");
    bd_fwupdate_install();
  } else {
    webconsole_print("Update available: " + entry.version + " (current " + fw_version +
                     ") - auto update disabled, install manually.");
  }
}
