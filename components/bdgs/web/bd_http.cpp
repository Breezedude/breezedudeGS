// HTTP server: static web UI, websocket endpoint, captive portal and the
// firmware / web-UI upload used by the fallback updater page.
//
// The API is described in docs/web-api.md.
#include <ArduinoJson.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Arduino.h"
#include "bd_auth.h"
#include "bd_console.h"
#include "bd_net.h"
#include "bd_power.h"
#include "bd_state.h"
#include "bd_storage.h"
#include "bd_web.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "http";

static httpd_handle_t s_server = nullptr;
// --- websocket endpoint -----------------------------------------------------
static esp_err_t ws_handler(httpd_req_t* req) {
  if (req->method == HTTP_GET) {
    // Handshake completed by the server itself.
    int fd = httpd_req_to_sockfd(req);
    bd_ws_out_add_client(fd);
    ESP_LOGI(TAG, "websocket client %d connected", fd);
    JsonDocument hello;
    hello["hello"]["proto"] = BD_WS_PROTO;
    hello["hello"]["fw"] = fw_version;
    String out;
    serializeJson(hello, out);
    bd_ws_send(fd, out);
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK) return err;

  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    bd_ws_out_remove_client(httpd_req_to_sockfd(req));
    return ESP_OK;
  }
  if (frame.len == 0 || frame.len > 4096) return ESP_OK;

  uint8_t* buffer = (uint8_t*)calloc(1, frame.len + 1);
  if (!buffer) return ESP_ERR_NO_MEM;
  frame.payload = buffer;
  err = httpd_ws_recv_frame(req, &frame, frame.len);
  if (err == ESP_OK && frame.type == HTTPD_WS_TYPE_TEXT) {
    bd_ws_handle_command(httpd_req_to_sockfd(req), (const char*)buffer, frame.len);
    // This is the task that sends, so the answer need not take the detour.
    bd_ws_out_flush();
  }
  free(buffer);
  return err;
}

// --- static files -----------------------------------------------------------
static const char* content_type_for(const char* path) {
  const char* dot = strrchr(path, '.');
  if (!dot) return "text/plain";
  if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
  if (!strcmp(dot, ".css")) return "text/css";
  if (!strcmp(dot, ".js")) return "application/javascript";
  if (!strcmp(dot, ".json")) return "application/json";
  if (!strcmp(dot, ".png")) return "image/png";
  if (!strcmp(dot, ".ico")) return "image/x-icon";
  if (!strcmp(dot, ".svg")) return "image/svg+xml";
  if (!strcmp(dot, ".woff2")) return "font/woff2";
  return "text/plain";
}

static bool send_file(httpd_req_t* req, const char* fs_path, bool gzipped) {
  FILE* file = fopen(fs_path, "rb");
  if (!file) return false;

  // The browser revalidates on every load and gets 304 while the installed UI
  // is unchanged. A plain max-age kept serving the old UI for minutes after a
  // firmware update, against a firmware with a different WebSocket protocol.
  char etag[16];
  snprintf(etag, sizeof(etag), "\"%08lx\"", (unsigned long)bd_storage_image_crc());
  char if_none_match[24] = {};
  if (httpd_req_get_hdr_value_str(req, "If-None-Match", if_none_match, sizeof(if_none_match)) == ESP_OK &&
      strcmp(if_none_match, etag) == 0) {
    fclose(file);
    httpd_resp_set_status(req, "304 Not Modified");
    httpd_resp_set_hdr(req, "ETag", etag);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, nullptr, 0);
    return true;
  }

  char type_path[160];
  strncpy(type_path, fs_path, sizeof(type_path) - 1);
  type_path[sizeof(type_path) - 1] = '\0';
  if (gzipped) {
    char* gz = strstr(type_path, ".gz");
    if (gz) *gz = '\0';
  }

  httpd_resp_set_type(req, content_type_for(type_path));
  if (gzipped) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "ETag", etag);
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

  char chunk[1024];
  size_t read_bytes;
  esp_err_t err = ESP_OK;
  while ((read_bytes = fread(chunk, 1, sizeof(chunk), file)) > 0) {
    err = httpd_resp_send_chunk(req, chunk, read_bytes);
    if (err != ESP_OK) break;
  }
  fclose(file);
  httpd_resp_send_chunk(req, nullptr, 0);
  return err == ESP_OK;
}

// Minimal page served when the www partition holds no UI. Deliberately
// self-contained (no external fonts or scripts) so it works on a station with
// no internet access at all. The station facts are inserted between the two
// halves by send_fallback_page().
static const char FALLBACK_HEAD[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Breezedude GS Recovery</title><style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;padding:24px;"
    "background:#101418;color:#e8edf2}h1{font-size:1.3rem}section{max-width:34rem;margin:0 auto}"
    "fieldset{border:1px solid #2b333c;border-radius:8px;margin:16px 0;padding:16px}"
    "legend{padding:0 6px;color:#9fb0c0}input,button{font:inherit}"
    "button{background:#2f6feb;color:#fff;border:0;border-radius:6px;padding:8px 14px;cursor:pointer}"
    "#log{white-space:pre-wrap;font-family:ui-monospace,monospace;font-size:.85rem;color:#9fb0c0}"
    "</style></head><body><section><h1>Breezedude Groundstation - Recovery</h1>"
    "<p>The web interface partition is empty or unreadable. Upload a new "
    "<code>www.bin</code> (web UI) or <code>breezedude_gs.bin</code> (firmware).</p>"
    "<fieldset><legend>Upload</legend>"
    "<p><input type=\"password\" id=\"p\" placeholder=\"Password, if one is set\"></p>"
    "<input type=\"file\" id=\"f\"><button onclick=\"up()\">Upload</button>"
    "<p id=\"log\"></p></fieldset>"
    "<fieldset><legend>Station</legend>";
static const char FALLBACK_TAIL[] =
    "<button onclick=\"rb()\">Reboot</button></fieldset>"
    "</section><script>"
    "function up(){var f=document.getElementById('f').files[0];if(!f)return;"
    "var d=new FormData();d.append('file',f);var x=new XMLHttpRequest();"
    "x.open('POST','/update');x.setRequestHeader('X-Password',pw());x.upload.onprogress=function(e){"
    "document.getElementById('log').textContent='Upload '+Math.round(e.loaded/e.total*100)+'%'};"
    "x.onload=function(){document.getElementById('log').textContent=x.responseText};x.send(d)}"
    "function pw(){return encodeURIComponent(document.getElementById('p').value)}"
    "function rb(){fetch('/reboot',{method:'POST',headers:{'X-Password':pw()}})"
    ".then(function(r){return r.text()}).then(function(t){document.getElementById('log').textContent=t})}"
    "</script></body></html>";

#if CONFIG_BDGS_FW_ALPHA_CHANNEL
static constexpr bool kAlphaBuild = true;
#else
static constexpr bool kAlphaBuild = false;
#endif

static void send_fallback_page(httpd_req_t* req) {
  // Same rule as the update check: a branch this build does not offer counts
  // as stable, so the page shows the channel the station really follows.
  settings_lock();
  const char* branch = update_branch_allowed(settings.updateBranch) ? settings.updateBranch : "stable";
  String facts = String("<p>Firmware: <b>") + fw_version + "</b>" +
                 (kAlphaBuild ? " (alpha build)" : "") +
                 "<br>Update branch: <b>" + branch + "</b>, auto update " +
                 (settings.autoUpdate ? "on" : "off") + "</p>";
  settings_unlock();

  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send_chunk(req, FALLBACK_HEAD, HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, facts.c_str(), facts.length());
  httpd_resp_send_chunk(req, FALLBACK_TAIL, HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t static_handler(httpd_req_t* req) {
  const char* uri = req->uri;
  char path[160];

  // Strip a query string and normalise "/" to the index page.
  size_t len = strcspn(uri, "?#");
  if (len == 1 && uri[0] == '/') {
    snprintf(path, sizeof(path), "%s/index.html", BD_WWW_BASE_PATH);
  } else if (len + sizeof(BD_WWW_BASE_PATH) + 4 >= sizeof(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
    return ESP_FAIL;
  } else {
    snprintf(path, sizeof(path), "%s%.*s", BD_WWW_BASE_PATH, (int)len, uri);
  }

  if (strstr(path, "..")) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    return ESP_FAIL;
  }

  if (bd_storage_mounted()) {
    char gz_path[176];
    snprintf(gz_path, sizeof(gz_path), "%s.gz", path);
    if (send_file(req, gz_path, true)) return ESP_OK;
    if (send_file(req, path, false)) return ESP_OK;
  }

  // Captive portal probes and anything else: send the device's own page so a
  // phone opens the configuration UI instead of an error.
  const bool is_index = strstr(path, "index.html") != nullptr;
  if (is_index) {
    send_fallback_page(req);
    return ESP_OK;
  }

  String location = "http://" + (bd_net_ap_active() ? bd_net_ap_ip() : bd_net_sta_ip()) + "/";
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location.c_str());
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

// --- firmware / web UI upload ----------------------------------------------
enum class UploadTarget { Unknown, Firmware, WebUi };

struct UploadState {
  UploadTarget target = UploadTarget::Unknown;
  esp_ota_handle_t ota = 0;
  const esp_partition_t* partition = nullptr;
  size_t written = 0;
  int last_percent = -5;
  bool failed = false;
  String message;
};

static bool upload_begin(UploadState* state, const String& filename, size_t total) {
  if (filename.indexOf("littlefs") >= 0 || filename.indexOf("www") >= 0 ||
      filename.indexOf("spiffs") >= 0) {
    state->target = UploadTarget::WebUi;
    state->partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                                BD_WWW_PARTITION_LABEL);
    if (!state->partition) {
      state->message = "no www partition found";
      return false;
    }
    if (total > state->partition->size) {
      state->message = String("image too large for the www partition (") + (unsigned)total + " > " +
                       (unsigned)state->partition->size + ")";
      return false;
    }
    bd_storage_unmount();
    if (esp_partition_erase_range(state->partition, 0, state->partition->size) != ESP_OK) {
      state->message = "erasing the www partition failed";
      return false;
    }
    webconsole_print("web UI upload started: " + filename + " (" + String((unsigned)total) +
                     " bytes)");
    return true;
  }

  state->target = UploadTarget::Firmware;
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (!next) {
    state->message = "no free OTA slot";
    return false;
  }
  if (total > next->size) {
    state->message = String("firmware too large for the OTA slot (") + (unsigned)total + " > " +
                     (unsigned)next->size + ")";
    return false;
  }
  // An image still pending verification may not start another OTA; one that
  // serves a firmware upload is healthy enough to confirm.
  esp_ota_mark_app_valid_cancel_rollback();
  if (esp_ota_begin(next, total, &state->ota) != ESP_OK) {
    state->message = "esp_ota_begin failed";
    return false;
  }
  state->partition = next;
  webconsole_print("firmware upload started: " + filename + " (" + String((unsigned)total) +
                   " bytes) -> " + next->label);
  return true;
}

static bool upload_write(UploadState* state, const uint8_t* data, size_t len, size_t total) {
  if (state->failed) return false;

  esp_err_t err = ESP_OK;
  if (state->target == UploadTarget::Firmware) {
    err = esp_ota_write(state->ota, data, len);
  } else if (state->target == UploadTarget::WebUi) {
    err = esp_partition_write(state->partition, state->written, data, len);
  }
  if (err != ESP_OK) {
    state->message = String("write failed: ") + esp_err_to_name(err);
    return false;
  }

  state->written += len;
  if (total > 0) {
    int percent = (int)((state->written * 100) / total);
    if (percent >= state->last_percent + 5) {
      state->last_percent = percent;
      String msg = String("{\"otaStatus\":\"") + percent + "%\"}";
      bd_ws_broadcast(msg);
    }
  }
  return true;
}

static bool upload_finish(UploadState* state) {
  if (state->target == UploadTarget::Firmware) {
    esp_err_t err = esp_ota_end(state->ota);
    if (err != ESP_OK) {
      state->message = String("image rejected: ") + esp_err_to_name(err);
      return false;
    }
    err = esp_ota_set_boot_partition(state->partition);
    if (err != ESP_OK) {
      state->message = String("setting the boot slot failed: ") + esp_err_to_name(err);
      return false;
    }
    webconsole_print("firmware image validated: " + String((unsigned)state->written) + " bytes");
    return true;
  }

  if (state->target == UploadTarget::WebUi) {
    bool mounted = bd_storage_mount();
    webconsole_print(String("web UI written: ") + (unsigned)state->written + " bytes, mount " +
                     (mounted ? "ok" : "FAILED"));
    return mounted;
  }

  state->message = "unknown upload target";
  return false;
}

// Streaming multipart/form-data reader. Only the first file part is consumed,
// which is all the UI ever sends.
// Changing something over HTTP needs what a websocket command needs: a session
// (X-Auth), or the password itself (X-Password, URL-encoded) for the recovery
// page, which has none.
static bool http_allowed(httpd_req_t* req) {
  if (!bd_auth_required()) return true;
  char value[200] = {};
  if (httpd_req_get_hdr_value_str(req, "X-Auth", value, sizeof(value)) == ESP_OK && bd_auth_allowed(value)) {
    return true;
  }
  if (httpd_req_get_hdr_value_str(req, "X-Password", value, sizeof(value)) != ESP_OK) return false;
  char password[sizeof(value)] = {};
  size_t n = 0;
  for (const char* p = value; *p && n < sizeof(password) - 1; p++) {
    if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
      const char hex[3] = {p[1], p[2], 0};
      password[n++] = (char)strtol(hex, nullptr, 16);
      p += 2;
    } else {
      password[n++] = *p;
    }
  }
  char token[BD_AUTH_TOKEN_LEN + 1];
  uint32_t retry_s = 0;
  return bd_auth_login(password, token, &retry_s);
}

static esp_err_t send_locked(httpd_req_t* req) {
  httpd_resp_set_status(req, "403 Forbidden");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_sendstr(req, "Locked: unlock in the web interface, or enter the password.");
  return ESP_OK;
}

static esp_err_t update_handler(httpd_req_t* req) {
  if (!http_allowed(req)) return send_locked(req);
  char content_type[128] = {0};
  httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));

  String boundary;
  const char* found = strstr(content_type, "boundary=");
  if (found) {
    String value(found + 9);
    // RFC 2046 allows the boundary to be quoted, and anything after a ';'
    // belongs to the next parameter.
    int semicolon = value.indexOf(';');
    if (semicolon >= 0) value = value.substring(0, semicolon);
    value.trim();
    if (value.length() >= 2 && value[0] == '"') {
      value = value.substring(1, value.length() - 1);
    }
    boundary = String("--") + value;
  }
  if (boundary.length() < 4) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "multipart boundary missing");
    return ESP_FAIL;
  }

  otaUploadInProgress = true;
  otaUploadRejected = false;
  bd_power_hold_awake(true);
  bd_net_bulk_transfer(true);

  UploadState state;
  String tail;        // bytes held back in case they start the terminator
  String headerAccum; // part headers, until the blank line
  bool in_data = false;
  size_t remaining = req->content_len;
  const size_t declared_total = req->content_len;

  char buffer[1024];
  while (remaining > 0) {
    int received = httpd_req_recv(req, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
      state.failed = true;
      state.message = "upload aborted by the client";
      break;
    }
    remaining -= received;

    if (!in_data) {
      headerAccum.str().append(buffer, received);
      int blank = headerAccum.indexOf("\r\n\r\n");
      if (blank < 0) {
        if (headerAccum.length() > 2048) {
          state.failed = true;
          state.message = "multipart header too long";
          break;
        }
        continue;
      }

      String headers = headerAccum.substring(0, blank);
      String filename = "firmware.bin";
      int namePos = headers.indexOf("filename=\"");
      if (namePos >= 0) {
        int end = headers.indexOf('"', namePos + 10);
        if (end > namePos) filename = headers.substring(namePos + 10, end);
      }

      // The payload is the body minus the part headers and the terminator.
      size_t overhead = (size_t)blank + 4 + boundary.length() + 6;
      size_t payload_estimate = declared_total > overhead ? declared_total - overhead : 0;
      if (!upload_begin(&state, filename, payload_estimate)) {
        state.failed = true;
        break;
      }

      in_data = true;
      String rest = headerAccum.substring(blank + 4);
      headerAccum.clear();
      tail = rest;
    } else {
      tail.str().append(buffer, received);
    }

    if (!in_data) continue;

    int terminator = tail.indexOf(String("\r\n") + boundary);
    if (terminator >= 0) {
      if (terminator > 0 &&
          !upload_write(&state, (const uint8_t*)tail.c_str(), (size_t)terminator,
                        declared_total)) {
        state.failed = true;
      }
      tail.clear();
      // Drain whatever is left so the connection stays usable.
      while (remaining > 0) {
        int drained = httpd_req_recv(req, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        if (drained <= 0) break;
        remaining -= drained;
      }
      break;
    }

    // Keep enough bytes to recognise a terminator split across two reads.
    size_t keep = boundary.length() + 4;
    if (tail.length() > keep) {
      size_t writable = tail.length() - keep;
      if (!upload_write(&state, (const uint8_t*)tail.c_str(), writable, declared_total)) {
        state.failed = true;
        break;
      }
      tail = tail.substring(writable);
    }
  }

  if (!state.failed && !tail.isEmpty()) {
    // No terminator seen (client sent a bare body): the remainder is payload.
    upload_write(&state, (const uint8_t*)tail.c_str(), tail.length(), declared_total);
  }

  bool ok = !state.failed && state.written > 0 && upload_finish(&state);

  if (!ok) {
    otaUploadRejected = true;
    if (state.message.isEmpty()) state.message = "upload failed";
    webconsole_print("update failed: " + state.message);
    bd_ws_broadcast(String("{\"otaStatus\":\"Update failed: ") + state.message + "\"}");
    if (state.target == UploadTarget::WebUi) bd_storage_mount();
    if (state.target == UploadTarget::Firmware && state.ota) esp_ota_abort(state.ota);
    otaUploadInProgress = false;
    bd_net_bulk_transfer(false);
    bd_power_hold_awake(false);
    String body = "Update Failed: " + state.message;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, body.c_str());
    return ESP_OK;
  }

  const bool reboot = state.target == UploadTarget::Firmware;
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_sendstr(req, reboot ? "Update Success. Rebooting..." : "Web UI updated.");

  if (reboot) {
    webconsole_print("update upload finished successfully - rebooting");
    bd_ws_broadcast("{\"otaStatus\":\"Update complete. Rebooting...\"}");
    bd_ws_broadcast("{\"disconnect\":\"true\"}");
    bd_ws_out_flush();
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
  }

  otaUploadInProgress = false;
  bd_net_bulk_transfer(false);
  bd_power_hold_awake(false);
  return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t* req) {
  if (!http_allowed(req)) return send_locked(req);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, "Rebooting...");
  webconsole_print("reboot requested over HTTP");
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
  return ESP_OK;
}

static esp_err_t partition_status_handler(httpd_req_t* req) {
  const esp_partition_t* fs = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, BD_WWW_PARTITION_LABEL);
  const esp_partition_t* running = esp_ota_get_running_partition();

  size_t total = 0, used = 0;
  bd_storage_info(&total, &used);

  char json[320];
  snprintf(json, sizeof(json),
           "{\"correct\":%s,\"fs_addr\":\"0x%06lX\",\"fs_size\":\"0x%06lX\",\"fs_used\":%u,"
           "\"web_ui\":%s,\"app\":\"%s\",\"app_addr\":\"0x%06lX\",\"app_size\":\"0x%06lX\"}",
           fs ? "true" : "false", fs ? (unsigned long)fs->address : 0UL,
           fs ? (unsigned long)fs->size : 0UL, (unsigned)used,
           bd_storage_has_web_ui() ? "true" : "false", running ? running->label : "?",
           running ? (unsigned long)running->address : 0UL,
           running ? (unsigned long)running->size : 0UL);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json);
  return ESP_OK;
}

// --- server -----------------------------------------------------------------
static void on_socket_close(httpd_handle_t, int sockfd) {
  bd_ws_out_remove_client(sockfd);
  close(sockfd);
}

bool bd_web_start() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_uri_handlers = 8;
  config.max_open_sockets = 7;
  config.lru_purge_enable = true;
  config.stack_size = 10240;
  config.recv_wait_timeout = 10;
  config.send_wait_timeout = 5;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.close_fn = on_socket_close;
  config.task_priority = 5;

  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
    return false;
  }

  bd_ws_out_begin(s_server);

  httpd_uri_t snapshot_uri = {};
  snapshot_uri.uri = "/api/snapshot";
  snapshot_uri.method = HTTP_GET;
  snapshot_uri.handler = bd_snapshot_handler;
  httpd_register_uri_handler(s_server, &snapshot_uri);

  httpd_uri_t ws_uri = {};
  ws_uri.uri = "/ws";
  ws_uri.method = HTTP_GET;
  ws_uri.handler = ws_handler;
  ws_uri.is_websocket = true;
  httpd_register_uri_handler(s_server, &ws_uri);

  httpd_uri_t update_uri = {};
  update_uri.uri = "/update";
  update_uri.method = HTTP_POST;
  update_uri.handler = update_handler;
  httpd_register_uri_handler(s_server, &update_uri);

  httpd_uri_t reboot_uri = {};
  reboot_uri.uri = "/reboot";
  reboot_uri.method = HTTP_POST;
  reboot_uri.handler = reboot_handler;
  httpd_register_uri_handler(s_server, &reboot_uri);

  httpd_uri_t status_uri = {};
  status_uri.uri = "/partition_status";
  status_uri.method = HTTP_GET;
  status_uri.handler = partition_status_handler;
  httpd_register_uri_handler(s_server, &status_uri);

  // Catch-all last: serves the UI and redirects captive portal probes.
  httpd_uri_t static_uri = {};
  static_uri.uri = "/*";
  static_uri.method = HTTP_GET;
  static_uri.handler = static_handler;
  httpd_register_uri_handler(s_server, &static_uri);

  ESP_LOGI(TAG, "web server up, web UI %s", bd_storage_has_web_ui() ? "from www" : "fallback page");
  return true;
}

void bd_web_stop() {
  if (!s_server) return;
  httpd_stop(s_server);
  s_server = nullptr;
}
