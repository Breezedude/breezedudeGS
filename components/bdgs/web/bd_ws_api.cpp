// Websocket command API and the JSON packers, see docs/web-api.md.
#include <ArduinoJson.h>
#include <ctype.h>

#include "Arduino.h"
#include "bd_auth.h"
#include "bd_console.h"
#include "bd_diag.h"
#include "bd_fwstore.h"
#include "bd_fwupdate.h"
#include "bd_net.h"
#include "bd_power.h"
#include "bd_radio.h"
#include "bd_repeat.h"
#include "bd_state.h"
#include "bd_storage.h"
#include "bd_web.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "helper.h"

static const char* TAG = "wsapi";

// Kconfig leaves a disabled bool undefined, so it cannot be used as a value.
#if CONFIG_BDGS_FW_ALPHA_CHANNEL
static constexpr bool kAlphaChannel = true;
#else
static constexpr bool kAlphaChannel = false;
#endif

// --- helpers ----------------------------------------------------------------
String formatMinutesAsTime(uint16_t totalMinutes) {
  char buffer[6];
  uint16_t normalized = totalMinutes % (24 * 60);
  snprintf(buffer, sizeof(buffer), "%02u:%02u", normalized / 60, normalized % 60);
  return String(buffer);
}

static uint16_t parseTimeToMinutes(JsonVariantConst value, uint16_t fallbackMinutes) {
  if (value.isNull()) return fallbackMinutes;

  String text;
  if (value.is<const char*>()) {
    text = String(value.as<const char*>());
  } else {
    return fallbackMinutes;
  }

  text.trim();
  if (text.length() != 5 || text[2] != ':') return fallbackMinutes;

  if (!isdigit((unsigned char)text[0]) || !isdigit((unsigned char)text[1]) ||
      !isdigit((unsigned char)text[3]) || !isdigit((unsigned char)text[4])) {
    return fallbackMinutes;
  }

  int hours = (text[0] - '0') * 10 + (text[1] - '0');
  int minutes = (text[3] - '0') * 10 + (text[4] - '0');
  if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) return fallbackMinutes;

  return (uint16_t)(hours * 60 + minutes);
}

bool hasRequiredSetupValues(const char* deviceName, float lat, float lon) {
  if (deviceName == nullptr) return false;
  bool nameChanged = strcmp(deviceName, "MyGS") != 0;
  bool latChanged = fabsf(lat - 47.0f) > 0.00001f;
  bool lonChanged = fabsf(lon - 12.0f) > 0.00001f;
  return nameChanged && latChanged && lonChanged;
}

bool isRequiredSetupComplete() {
  return hasRequiredSetupValues(settings.deviceName, settings.latitude, settings.longitude);
}

String getFormattedUptime(uint32_t uptimeSeconds) {
  unsigned int days = uptimeSeconds / 86400;
  uptimeSeconds = uptimeSeconds % 86400;
  unsigned int hours = uptimeSeconds / 3600;
  uptimeSeconds = uptimeSeconds % 3600;
  unsigned int minutes = uptimeSeconds / 60;

  char buffer[25];
  if (days > 0) {
    snprintf(buffer, sizeof(buffer), "%ud %uh %um", days, hours, minutes);
  } else if (hours > 0) {
    snprintf(buffer, sizeof(buffer), "%uh %um", hours, minutes);
  } else {
    snprintf(buffer, sizeof(buffer), "%um", minutes);
  }
  return String(buffer);
}

// --- JSON packers -----------------------------------------------------------
void fillWeather(JsonObject w, weatherData* wt) {
  w["vid"] = wt->vid;
  w["fanet_id"] = wt->fanet_id;
  w["name"] = wt->name;
  w["rssi"] = wt->rssi;
  w["snr"] = wt->snr;
  w["lat"] = wt->lat;
  w["lon"] = wt->lon;
  w["tLastMsg"] = wt->timestamp;

  if (wt->bTemp) w["temp"] = wt->temp;
  if (wt->bWind) {
    w["wHeading"] = wt->wHeading;
    w["wSpeed"] = wt->wSpeed;
    w["wGust"] = wt->wGust;
  }
  if (wt->bHumidity) w["humidity"] = wt->Humidity;
  if (wt->bBaro) w["baro"] = wt->Baro;
  if (wt->bStateOfCharge) w["charge"] = wt->Charge;
}

// A packet that is not built is a packet the clients have to catch up on.
static bool live_packet_skipped() {
  if (!bd_ws_heap_low()) return false;
  bd_ws_note_dropped();
  return true;
}

String packWeather(weatherData* wt) {
  if (live_packet_skipped()) return String();
  JsonDocument resp;
  fillWeather(resp["weather"].to<JsonArray>().add<JsonObject>(), wt);
  String output;
  serializeJson(resp, output);
  return output;
}

void fillTracking(JsonObject t, trackingData* td) {
  t["vid"] = td->vid;
  t["name"] = td->name;
  t["fanet_id"] = td->fanet_id;
  t["rssi"] = td->rssi;
  t["snr"] = td->snr;
  t["lat"] = td->lat;
  t["lon"] = td->lon;
  t["alt"] = td->alt;
  t["hdop"] = td->hdop;
  t["acft"] = trck_acft_names[td->aircraftType];
  t["spd"] = td->speed;
  t["climb"] = td->climb;
  t["heading"] = td->heading;
  t["track"] = td->onlineTracking;
  t["tLastMsg"] = td->timestamp;
  t["src"] = (td->srcProto == TRACK_SRC_FLARM)  ? "FLARM"
             : (td->srcProto == TRACK_SRC_ADSL) ? "ADS-L"
                                                : "FANET";
  t["notrack"] = td->noTrack;
  if (td->state < 17) {
    t["state"] = trck_state_names[td->state];
  } else {
    t["state"] = "-";
  }

  // FANET ground tracking packets carry no motion data. FLARM and ADS-L always
  // do, even for an aircraft sitting on the ground, so they keep their values.
  if (td->srcProto == TRACK_SRC_FANET && td->state < 16) {
    t["spd"] = "-";
    t["climb"] = "-";
    t["alt"] = "-";
    t["heading"] = "-";
    t["acft"] = "-";
  }
}

String packTracking(trackingData* td) {
  if (live_packet_skipped()) return String();
  JsonDocument resp;
  fillTracking(resp["tracking"].to<JsonArray>().add<JsonObject>(), td);
  String output;
  serializeJson(resp, output);
  return output;
}

String bytesToHex(const uint8_t* data, size_t len) {
  String out;
  out.reserve(len * 2);
  char hex[3];
  for (size_t i = 0; i < len; i++) {
    snprintf(hex, sizeof(hex), "%02X", data[i]);
    out += hex;
  }
  return out;
}

void fillHwInfo(JsonObject h, const hwInfoData* hi) {
  h["vid"] = hi->vid;
  h["fanet_id"] = hi->fanet_id;
  h["name"] = getDeviceName(hi->vid, hi->fanet_id);
  h["devId"] = hi->devId;
  h["rssi"] = hi->rssi;
  h["snr"] = hi->snr;
  h["tLastMsg"] = hi->timestamp;
  h["tNewestHwInfo"] = getNewestHwInfoTimestamp(hi->vid, hi->fanet_id);

  h["subHeader"] = hi->subHeader;
  h["pingPongRequest"] = hi->pingPongRequest;
  h["hasSubtypeBuildDate"] = hi->hasSubtypeBuildDate;
  h["hasIcaoAddress"] = hi->hasIcaoAddress;
  h["hasUptime"] = hi->hasUptime;
  h["hasRxRssi"] = hi->hasRxRssi;
  h["hasExtendedHeader"] = hi->hasExtendedHeader;

  h["deviceType"] = hi->deviceType;
  h["isDevelopmentBuild"] = hi->isDevelopmentBuild;
  h["buildYear"] = hi->buildYear;
  h["buildMonth"] = hi->buildMonth;
  h["buildDay"] = hi->buildDay;

  if (hi->hasIcaoAddress) h["icaoAddress"] = hi->icaoAddress;
  if (hi->hasUptime) h["uptimeMinutes"] = hi->uptimeMinutes;
  if (hi->hasRxRssi) {
    h["rxRssiDbm"] = hi->rxRssiDbm;
    h["rxRssiSourceVendor"] = hi->rxRssiSourceVendor;
    h["rxRssiSourceAddress"] = hi->rxRssiSourceAddress;
  }
  if (hi->hasExtendedHeader) h["extHeader"] = hi->extHeader;

  h["rawLen"] = hi->rawLen;
  h["rawHex"] = bytesToHex(hi->rawAfterBuildDate, hi->rawLen);

  JsonArray rawArray = h["rawBytes"].to<JsonArray>();
  for (uint8_t i = 0; i < hi->rawLen; i++) {
    rawArray.add(hi->rawAfterBuildDate[i]);
  }
}

String packHwInfo(const hwInfoData* hi) {
  if (live_packet_skipped()) return String();
  JsonDocument resp;
  fillHwInfo(resp["hwinfo"].to<JsonArray>().add<JsonObject>(), hi);
  String output;
  serializeJson(resp, output);
  return output;
}

// Which bootloader the station runs. Stations migrated from the Arduino
// firmware keep its ESP-IDF 4.4 bootloader, which predates the description
// structure - so "not found" identifies them reliably.
static String bootloader_text() {
  esp_bootloader_desc_t desc = {};
  esp_err_t err = esp_ota_get_bootloader_description(nullptr, &desc);
  if (err == ESP_OK) {
    desc.idf_ver[sizeof(desc.idf_ver) - 1] = '\0';
    return String("ESP-IDF ") + desc.idf_ver;
  }
  if (err == ESP_ERR_NOT_FOUND) return "legacy (Arduino, ESP-IDF 4.x)";
  return "unknown";
}

static const char* reset_reason_text(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT: return "brown-out";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    default: return "unknown";
  }
}

static const char* radio_module_text() {
  switch (lora_module) {
    case RADIO_MODULE_SX1276: return "SX1276";
    case RADIO_MODULE_LLCC68: return "LLCC68";
    case RADIO_MODULE_SX1262: return "SX1262";
    default: return "none";
  }
}

size_t bd_ws_status_json(char* out, size_t cap) {
  JsonDocument resp;
  resp["type"] = "status";
  resp["time"] = (uint32_t)time(nullptr);
  resp["uptime"] = (uint32_t)(millis() / 1000);
  resp["heap"] = ESP.getFreeHeap();
  resp["heapMin"] = (uint32_t)esp_get_minimum_free_heap_size();
  resp["heapBlock"] = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  resp["sleep"] = bd_power_state_text();

  const BdWsOutStats wsOut = bd_ws_out_stats();
  JsonObject ws = resp["ws"].to<JsonObject>();
  ws["clients"] = wsOut.clients;
  ws["dropped"] = wsOut.dropped;
  ws["resyncs"] = wsOut.resyncs;
  ws["oversize"] = wsOut.oversize;
  ws["jobsLost"] = wsOut.jobsLost;

  JsonObject wifi = resp["wifi"].to<JsonObject>();
  wifi["sta"] = bd_net_sta_connected();
  wifi["ssid"] = settings.wifi_ssid;
  wifi["ip"] = bd_net_sta_ip();
  wifi["rssi"] = bd_net_sta_rssi();
  wifi["ap"] = bd_net_ap_active();
  if (bd_net_ap_active()) {
    wifi["apIp"] = bd_net_ap_ip();
    wifi_sta_list_t clients = {};
    if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK) wifi["apClients"] = clients.num;
  }
  resp["internet"] = getInternetConnectionStatus() == "ok";
  resp["aprs"] = (!settings.sendAPRS) ? "off" : (aprs.connected() ? "connected" : "disconnected");
  if (settings.sendBreezedude) {
    uint32_t age = 0;
    int code = bd_net_last_forward(&age);
    JsonObject bd = resp["breezedude"].to<JsonObject>();
    bd["code"] = code;
    if (code != 0) bd["age"] = age;
  }

  const BdDiagState diag = bd_diag_state();
  JsonObject diagOut = resp["diag"].to<JsonObject>();
  diagOut["enabled"] = diag.enabled;
  diagOut["accepted"] = diag.accepted;
  diagOut["rejected"] = diag.rejected;
  if (diag.lastTs) {
    diagOut["lastTs"] = diag.lastTs;
    diagOut["lastCode"] = diag.lastCode;
  }

  JsonObject update = resp["update"].to<JsonObject>();
  update["available"] = update_available;
  update["version"] = update_available_version;

  JsonObject radio = resp["radio"].to<JsonObject>();
  radio["fanet"] = settings.receiveFanet;
  radio["fanetRx"] = fanet_rx_count;
  if (settings.receiveFlarm) {
    JsonObject flarm = radio["flarm"].to<JsonObject>();
    flarm["state"] = flarm_enabled() ? "on" : "no position";
    flarm["decoded"] = flarm_decoded;
    flarm["frames"] = flarm_rx_frames;
    flarm["crcFail"] = flarm_crc_errors;
    flarm["decodeFail"] = flarm_decode_fail;
  }
  if (settings.receiveAdsl) {
    JsonObject adsl = radio["adsl"].to<JsonObject>();
    adsl["state"] = adsl_enabled() ? "on" : "off";
    adsl["decoded"] = adsl_decoded;
    adsl["frames"] = adsl_rx_frames;
    adsl["crcFail"] = adsl_crc_errors;
    adsl["decodeFail"] = adsl_decode_fail;
  }

  if (settings.batteryPowered) {
    float voltage = getBatteryVoltage();
    if (!isnan(voltage)) resp["battery"] = roundf(voltage * 1000.0f) / 1000.0f;
  }

  const size_t len = serializeJson(resp, out, cap);
  return len < cap ? len : 0;
}

// --- command dispatcher -----------------------------------------------------
static void reply(int fd, JsonDocument& doc) {
  String output;
  serializeJson(doc, output);
  bd_ws_send(fd, output);
}

// Commands that change something. While a password is set they need a session,
// see core/bd_auth.cpp; reading stays open.
static bool changes_something(const char* cmd) {
  static const char* const kCommands[] = {
      "save_settings", "save_wifi",         "wifi_scan",         "check_update",     "reboot",
      "repeat_reset",  "set_ota_verbose",   "set_flarm_verbose", "set_adsl_verbose", "set_password",
      "diag_claim"};
  for (const char* c : kCommands) {
    if (strcmp(cmd, c) == 0) return true;
  }
  return false;
}

static void send_session(int fd, const char* token) {
  JsonDocument resp;
  resp["auth"]["token"] = token;
  resp["auth"]["expiresIn"] = BD_AUTH_SESSION_S;
  reply(fd, resp);
}

void bd_ws_handle_command(int fd, const char* data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    bd_ws_send(fd, "{\"error\":\"Invalid JSON\"}");
    return;
  }

  const char* cmd = doc["cmd"];
  if (!cmd) {
    bd_ws_send(fd, "{\"error\":\"Missing command\"}");
    return;
  }

  // The Arduino firmware's page, still cached by a browser after the migration:
  // its server sent Last-Modified without Cache-Control, which browsers may keep
  // for weeks. It shows "deviceNamedisplay" in its header and "error" in red,
  // so that is where it learns to reload. A message with "error" is shown as
  // nothing else, hence two. It polls get_info_update and get_fanet every
  // second, which would otherwise answer "Unknown command".
  if (strcmp(cmd, "get_info") == 0 || strcmp(cmd, "get_wifi") == 0 || strcmp(cmd, "get_settings") == 0 ||
      strcmp(cmd, "get_info_update") == 0 || strcmp(cmd, "get_fanet") == 0) {
    bd_ws_send(fd, "{\"deviceNamedisplay\":\"This page is outdated - please reload it (Ctrl+F5)\"}");
    bd_ws_send(fd, "{\"error\":\"Outdated page - please reload it (Ctrl+F5)\"}");
    return;
  }

  const char* token = doc["auth"] | "";
  const bool allowed = bd_auth_allowed(token);
  if (changes_something(cmd) && !allowed) {
    bd_ws_send(fd, "{\"error\":\"Locked - unlock to make changes\",\"locked\":true}");
    return;
  }

  if (strcmp(cmd, "login") == 0) {
    char session[BD_AUTH_TOKEN_LEN + 1];
    uint32_t retry_s = 0;
    if (bd_auth_login(doc["password"] | "", session, &retry_s)) {
      send_session(fd, session);
    } else {
      JsonDocument resp;
      resp["error"] = retry_s ? String("Too many wrong passwords - try again in ") + retry_s + " s"
                              : String("Wrong password");
      resp["locked"] = true;
      reply(fd, resp);
    }
    return;
  }

  // Whether this client's session still holds, asked when the page loads.
  if (strcmp(cmd, "auth_check") == 0) {
    uint32_t left_s = 0;
    JsonDocument resp;
    resp["auth"]["required"] = bd_auth_required();
    resp["auth"]["valid"] = allowed;
    if (allowed && bd_auth_allowed(token, &left_s) && left_s) resp["auth"]["expiresIn"] = left_s;
    reply(fd, resp);
    return;
  }

  if (strcmp(cmd, "auth_button") == 0) {
    bd_auth_button_request(fd);
    JsonDocument resp;
    resp["authButton"] = BD_AUTH_BUTTON_WINDOW_S;
    reply(fd, resp);
    return;
  }

  if (strcmp(cmd, "logout") == 0) {
    bd_auth_logout(token);
    bd_ws_send(fd, "{\"auth\":{\"valid\":false,\"required\":true}}");
    return;
  }

  // An empty password removes it. Whoever sets the first one keeps a session.
  if (strcmp(cmd, "set_password") == 0) {
    const char* password = doc["password"] | "";
    const bool hadSession = bd_auth_required();
    bd_auth_set_password(password, hadSession ? token : nullptr);
    char session[BD_AUTH_TOKEN_LEN + 1];
    uint32_t retry_s = 0;
    if (password[0] && !hadSession && bd_auth_login(password, session, &retry_s)) send_session(fd, session);
    bd_ws_send(fd, password[0] ? "{\"msg\":\"Password set\"}" : "{\"msg\":\"Password removed\"}");
    JsonDocument resp;
    resp["auth"]["required"] = bd_auth_required();
    resp["auth"]["valid"] = true;
    reply(fd, resp);
    return;
  }

  // Static facts, requested once per connection.
  if (strcmp(cmd, "info") == 0) {
    JsonDocument resp;
    resp["type"] = "info";
    resp["version"] = fw_version;
    resp["alphaChannel"] = kAlphaChannel;
    resp["compileTime"] = __DATE__ " " __TIME__;
    resp["idf"] = esp_get_idf_version();
    resp["bootloader"] = bootloader_text();
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    resp["chip"] = CONFIG_IDF_TARGET;
    resp["chipRev"] = chip.revision;
    resp["cores"] = chip.cores;
    uint32_t flashSize = 0;
    if (esp_flash_get_size(nullptr, &flashSize) == ESP_OK) resp["flashSize"] = flashSize;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) resp["partition"] = running->label;
    resp["resetReason"] = reset_reason_text(esp_reset_reason());
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
      char macText[18];
      snprintf(macText, sizeof(macText), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
               mac[3], mac[4], mac[5]);
      resp["mac"] = macText;
    }
    resp["radio"] = radio_module_text();
    uint32_t storeTotal = 0, storeFree = 0;
    bd_fwstore_space(&storeTotal, &storeFree);
    resp["fwstore"]["total"] = storeTotal;
    resp["fwstore"]["free"] = storeFree;
    reply(fd, resp);
  }

  // Everything that changes, polled every few seconds by the dashboard.
  else if (strcmp(cmd, "status") == 0) {
    bd_ws_request_status(fd);
  }

  // All settings, WiFi included, in one message.
  else if (strcmp(cmd, "config") == 0) {
    JsonDocument resp;
    resp["type"] = "config";
    settings_lock();
    resp["deviceName"] = settings.deviceName;
    resp["lon"] = settings.longitude;
    resp["lat"] = settings.latitude;
    resp["elevation"] = settings.elevation;
    resp["batteryPowered"] = settings.batteryPowered;
    resp["solarPowered"] = settings.solarPowered;
    resp["sleepScheduleEnabled"] = settings.sleepScheduleEnabled;
    resp["sleepOffTime"] = formatMinutesAsTime(settings.sleepOffMinutes);
    resp["sleepOnTime"] = formatMinutesAsTime(settings.sleepOnMinutes);
    resp["sendAPRS"] = settings.sendAPRS;
    resp["aprsServer"] = settings.aprsServer;
    resp["aprsPort"] = settings.aprsPort;
    resp["sendBreezedude"] = settings.sendBreezedude;
    resp["sendDiagnostics"] = !settings.disableDiagnostics;
    resp["receiveFlarm"] = settings.receiveFlarm;
    resp["receiveAdsl"] = settings.receiveAdsl;
    resp["receiveFanet"] = settings.receiveFanet;
    resp["flarmDutyProfile"] = settings.flarmDutyProfile;
    resp["autoUpdate"] = settings.autoUpdate;
    resp["updateBranch"] = settings.updateBranch;
    resp["ap_ssid"] = settings.ap_ssid;
    resp["sta_ssid"] = settings.wifi_ssid;
    // Readable for everyone except the passwords, which need a session.
    if (allowed) {
      resp["ap_password"] = settings.ap_password;
      resp["sta_password"] = settings.wifi_password;
    }
    resp["keepAP"] = settings.keepAP;
    resp["apAutoOff"] = settings.apAutoOff;
    settings_unlock();
    resp["authRequired"] = bd_auth_required();
    reply(fd, resp);
    bd_ws_send(fd, bd_fwupdate_channels_json());
  }

  else if (strcmp(cmd, "repeat_reset") == 0) {
    bd_repeat_reset();
    bd_ws_broadcast("{\"repeat_reset\":true}");
  }

  // Linking to an account in the device portal; the HTTPS request runs in the
  // net task, which answers with {"diag_claim":{...}}.
  else if (strcmp(cmd, "diag_claim") == 0) {
    bd_diag_request_claim(fd);
    bd_net_reevaluate();
  }

  else if (strcmp(cmd, "check_update") == 0) {
    force_update_check = true;
    bd_net_reevaluate();
    bd_ws_send(fd, "{\"msg\":\"Checking for firmware updates...\"}");
  }

  else if (strcmp(cmd, "save_wifi") == 0) {
    // Use | "" fallback so missing keys never yield a nullptr that would crash
    // strcmp / strncpy.
    const char* new_sta_ssid = doc["sta_ssid"] | "";
    const char* new_sta_pass = doc["sta_password"] | "";
    const char* new_ap_ssid = doc["ap_ssid"] | "";
    const char* new_ap_pass = doc["ap_password"] | "";
    bool new_keepAP = doc["keepAP"].as<bool>();
    // The two AP options exclude each other; "always on" wins if both arrive.
    bool new_apAutoOff = doc["apAutoOff"].as<bool>() && !new_keepAP;

    settings_lock();
    bool autoOffSwitchedOn = new_apAutoOff && !settings.apAutoOff;
    settings.apAutoOff = new_apAutoOff;
    bool staChanged = (strcmp(settings.wifi_ssid, new_sta_ssid) != 0) ||
                      (strcmp(settings.wifi_password, new_sta_pass) != 0);
    bool apChanged = (strcmp(settings.ap_ssid, new_ap_ssid) != 0) ||
                     (strcmp(settings.ap_password, new_ap_pass) != 0);

    strncpy(settings.wifi_ssid, new_sta_ssid, sizeof(settings.wifi_ssid));
    settings.wifi_ssid[sizeof(settings.wifi_ssid) - 1] = '\0';
    strncpy(settings.wifi_password, new_sta_pass, sizeof(settings.wifi_password));
    settings.wifi_password[sizeof(settings.wifi_password) - 1] = '\0';
    strncpy(settings.ap_ssid, new_ap_ssid, sizeof(settings.ap_ssid));
    settings.ap_ssid[sizeof(settings.ap_ssid) - 1] = '\0';
    strncpy(settings.ap_password, new_ap_pass, sizeof(settings.ap_password));
    settings.ap_password[sizeof(settings.ap_password) - 1] = '\0';
    settings.keepAP = new_keepAP;
    settings_unlock();
    save_preferences();

    // Switching auto-off on starts a fresh window, so the AP does not vanish
    // the moment the setting is saved; any other change just re-runs the policy.
    if (autoOffSwitchedOn) {
      bd_net_ap_activate("auto-off enabled");
    } else {
      bd_net_reevaluate();
    }

    ESP_LOGI(TAG, "save_wifi: ssid='%s' keepAP=%d staChanged=%d apChanged=%d", new_sta_ssid,
             (int)new_keepAP, (int)staChanged, (int)apChanged);

    if (apChanged) {
      bd_net_request_ap_restart();
      bd_ws_send(fd, "{\"msg\":\"AP settings saved. Restarting AP...\"}");
    }

    if (staChanged) {
      bd_net_request_sta_reconnect();
      bd_ws_send(fd, "{\"msg\":\"WiFi settings saved. Reconnecting STA...\"}");
    } else if (bd_net_sta_connected()) {
      JsonDocument resp;
      resp["msg"] = String("WiFi connected. IP: ") + bd_net_sta_ip();
      reply(fd, resp);
    } else {
      bd_ws_send(fd, "{\"msg\":\"WiFi settings saved. STA currently not connected.\"}");
    }
  }

  else if (strcmp(cmd, "wifi_scan") == 0) {
    // The result arrives as {"wifi_scan":[...]} or {"wifi_scan_error":...}.
    if (!bd_net_scan_start()) bd_ws_send(fd, "{\"msg\":\"Scan already running...\"}");
  }

  else if (strcmp(cmd, "save_settings") == 0) {
    const char* new_deviceName = doc["deviceName"] | "";
    const char* new_aprsServer = doc["aprsServer"] | "";
    const char* new_updateBranch = doc["updateBranch"] | "stable";
    if (!update_branch_allowed(new_updateBranch)) new_updateBranch = "stable";

    settings_lock();
    bool updateBranchChanged = strcmp(settings.updateBranch, new_updateBranch) != 0;

    strncpy(settings.deviceName, new_deviceName, sizeof(settings.deviceName));
    settings.deviceName[sizeof(settings.deviceName) - 1] = '\0';
    strncpy(settings.aprsServer, new_aprsServer, sizeof(settings.aprsServer));
    settings.aprsServer[sizeof(settings.aprsServer) - 1] = '\0';

    settings.longitude = doc["lon"].as<float>();
    settings.latitude = doc["lat"].as<float>();
    settings.elevation = doc["elevation"].as<int>();
    settings.batteryPowered = doc["batteryPowered"].as<bool>();
    // Solar only describes how the battery is charged.
    settings.solarPowered = settings.batteryPowered && doc["solarPowered"].as<bool>();
    settings.sleepScheduleEnabled =
        settings.batteryPowered && doc["sleepScheduleEnabled"].as<bool>();
    settings.sleepOffMinutes = parseTimeToMinutes(doc["sleepOffTime"], settings.sleepOffMinutes);
    settings.sleepOnMinutes = parseTimeToMinutes(doc["sleepOnTime"], settings.sleepOnMinutes);

    bool requiredSetupComplete =
        hasRequiredSetupValues(settings.deviceName, settings.latitude, settings.longitude);
    bool requestedSendAprs = doc["sendAPRS"].as<bool>();
    bool requestedSendBreezedude = doc["sendBreezedude"].as<bool>();
    bool setupIncomplete = !requiredSetupComplete && (requestedSendAprs || requestedSendBreezedude);
    if (setupIncomplete) {
      settings.sendAPRS = false;
      settings.sendBreezedude = false;
    } else {
      settings.sendAPRS = requestedSendAprs;
      settings.sendBreezedude = requestedSendBreezedude;
    }

    // FLARM decoding reconstructs positions relative to the station, so it is
    // pointless without a proper position and stays off until one is set.
    settings.receiveFlarm = requiredSetupComplete && doc["receiveFlarm"].as<bool>();
    // ADS-L carries absolute coordinates, so unlike FLARM it does not depend on
    // the station position being configured.
    settings.receiveAdsl = doc["receiveAdsl"].as<bool>();
    settings.receiveFanet = doc["receiveFanet"].as<bool>();
    uint8_t requestedProfile = doc["flarmDutyProfile"].as<uint8_t>();
    settings.flarmDutyProfile = (requestedProfile <= 2) ? requestedProfile : 0;
    settings.aprsPort = doc["aprsPort"].as<int>();
    settings.disableDiagnostics = !(doc["sendDiagnostics"] | true);
    settings.autoUpdate = doc["autoUpdate"].as<bool>();
    strncpy(settings.updateBranch, new_updateBranch, sizeof(settings.updateBranch));
    settings.updateBranch[sizeof(settings.updateBranch) - 1] = '\0';
    settings_unlock();

    save_preferences();

    if (setupIncomplete) {
      bd_ws_send(fd,
                 "{\"msg\":\"APRS und Send-to-Breezedude bleiben deaktiviert: Bitte Name sowie "
                 "Latitude/Longitude sorgsam setzen und speichern.\"}");
    }
    bd_ws_send(fd, "{\"msg\":\"Settings saved\"}");

    update_aprs_settings();
    configureTimeZoneFromLocation();
    // The radio task re-reads which protocols are wanted on the next pass.
    bd_radio_notify();

    if (updateBranchChanged) {
      bd_ws_send(fd, "{\"msg\":\"Update branch changed. Checking for updates...\"}");
      onUpdateBranchChanged();
    }
  }

  else if (strcmp(cmd, "set_ota_verbose") == 0) {
    ota_verbose_ws = doc["verbose"] | false;
    JsonDocument resp;
    resp["ota_verbose"] = ota_verbose_ws;
    reply(fd, resp);
  }

  else if (strcmp(cmd, "set_adsl_verbose") == 0) {
    adsl_verbose_ws = doc["verbose"] | false;
    JsonDocument resp;
    resp["adsl_verbose"] = adsl_verbose_ws;
    if (adsl_verbose_ws && !adsl_enabled()) {
      resp["msg"] = "ADS-L-Empfang ist aus - es kommen keine Frames.";
    } else if (adsl_verbose_ws) {
      resp["msg"] = "ADS-L-Debug an: Funk bleibt dauerhaft auf FSK, FANET-Empfang pausiert.";
    }
    bd_radio_notify();
    reply(fd, resp);
  }

  else if (strcmp(cmd, "set_flarm_verbose") == 0) {
    flarm_verbose_ws = doc["verbose"] | false;
    JsonDocument resp;
    resp["flarm_verbose"] = flarm_verbose_ws;
    if (flarm_verbose_ws && !flarm_enabled()) {
      resp["msg"] =
          "FLARM-Empfang ist aus (oder die Stationsposition fehlt) - es kommen keine Frames.";
    } else if (flarm_verbose_ws) {
      resp["msg"] = "FLARM-Debug an: Funk bleibt dauerhaft auf FSK, FANET-Empfang pausiert.";
    }
    bd_radio_notify();
    reply(fd, resp);
  }

  else if (strcmp(cmd, "reboot") == 0) {
    bd_ws_send(fd, "{\"msg\":\"rebooting...\"}");
    bd_ws_send(fd, "{\"disconnect\":\"true\"}");
    bd_ws_out_flush();
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
  }

  else {
    bd_ws_send(fd, "{\"error\":\"Unknown command\"}");
  }
}
