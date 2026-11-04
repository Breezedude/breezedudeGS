#include "bd_console.h"

#include <ArduinoJson.h>

#include "bd_json_out.h"
#include "bd_web.h"
#include "bd_state.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "console";

static const size_t WEB_CONSOLE_HISTORY_SIZE = 20;
struct WebConsoleEntry {
  time_t ts;
  String msg;
};
static WebConsoleEntry s_history[WEB_CONSOLE_HISTORY_SIZE];
static size_t s_history_count = 0;
static size_t s_history_head = 0;
static SemaphoreHandle_t s_history_mutex = nullptr;

// Normalize log payload in a single pass to avoid repeated String scans.
String sanitizeLogLine(const String& input) {
  String out;
  out.reserve(input.length());

  bool sawNonWhitespace = false;
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '\r' || c == '\n') {
      c = ' ';
    }

    if (!sawNonWhitespace) {
      if (c == ' ' || c == '\t') {
        continue;
      }
      sawNonWhitespace = true;
    }

    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) {
      c = '?';
    }

    out += c;
  }

  while (out.length() > 0) {
    char c = out[out.length() - 1];
    if (c == ' ' || c == '\t') {
      out.remove(out.length() - 1);
    } else {
      break;
    }
  }

  return out;
}

static void history_push(time_t ts, const String& msg) {
  if (msg.length() == 0) return;
  if (!s_history_mutex) s_history_mutex = xSemaphoreCreateMutex();

  xSemaphoreTake(s_history_mutex, portMAX_DELAY);
  s_history[s_history_head].ts = ts;
  s_history[s_history_head].msg = msg;
  s_history_head = (s_history_head + 1) % WEB_CONSOLE_HISTORY_SIZE;
  if (s_history_count < WEB_CONSOLE_HISTORY_SIZE) {
    s_history_count++;
  }
  xSemaphoreGive(s_history_mutex);
}

static void push_line(const char* key, const String& text, bool with_timestamp) {
  if (!bd_ws_has_clients()) return;
  if (bd_ws_heap_low()) {
    bd_ws_note_dropped();
    return;
  }
  JsonDocument doc;
  doc[key] = text;
  if (with_timestamp) doc["ts"] = (uint32_t)time(nullptr);
  String out;
  serializeJson(doc, out);
  bd_ws_broadcast(out);
}

void webconsole_print(String in) {
  in = sanitizeLogLine(in);
  if (in.length() == 0) return;

  // Mirrored to the serial console: in the field the web UI is often the only
  // window into the station, on the bench it is the serial port.
  ESP_LOGI(TAG, "%s", in.c_str());

  time_t ts = time(nullptr);
  history_push(ts, in);
  push_line("webconsole", in, true);
}

void ota_status_v(const String& msg) {
  if (!ota_verbose_ws) return;
  String in = sanitizeLogLine(msg);
  if (in.length() == 0) return;
  push_line("webconsole", in, true);
}

void aprsconsole_print(String in) {
  in = sanitizeLogLine(in);
  if (in.length() == 0) return;
  push_line("aprsconsole", in, false);
}

// Goes straight to the connected clients and the serial port, bypassing the
// history so per-packet noise does not push everything else out.
void flarm_debug_print(const String& msg) {
  ESP_LOGI(TAG, "%s", msg.c_str());
  push_line("webconsole", msg, true);
}

void ota_progress_start(uint32_t totalChunks) {
  if (!bd_ws_has_clients()) return;
  JsonDocument doc;
  JsonObject p = doc["ota_progress"].to<JsonObject>();
  p["phase"] = "start";
  p["total"] = totalChunks;
  String out;
  serializeJson(doc, out);
  bd_ws_broadcast(out);
}

void ota_progress_chunk(uint16_t seq, uint8_t retries, float snr, uint8_t ackResendCount,
                        int8_t deviceSnr, int8_t deviceRssi, uint8_t proactiveAckResendCount) {
  if (!bd_ws_has_clients()) return;
  JsonDocument doc;
  JsonObject p = doc["ota_progress"].to<JsonObject>();
  p["phase"] = "chunk";
  p["seq"] = seq;
  p["retries"] = retries;
  p["snr"] = snr;
  p["ackResends"] = ackResendCount;
  p["deviceSnr"] = deviceSnr;
  p["deviceRssi"] = deviceRssi;
  p["proactiveAckResends"] = proactiveAckResendCount;
  String out;
  serializeJson(doc, out);
  bd_ws_broadcast(out);
}

void ota_progress_end(bool success) {
  if (!bd_ws_has_clients()) return;
  JsonDocument doc;
  JsonObject p = doc["ota_progress"].to<JsonObject>();
  p["phase"] = "end";
  p["success"] = success;
  String out;
  serializeJson(doc, out);
  bd_ws_broadcast(out);
}

void webconsole_history_write(BdJsonOut& out) {
  if (!s_history_mutex) return;

  for (size_t i = 0; i < WEB_CONSOLE_HISTORY_SIZE && out.ok(); i++) {
    JsonDocument doc;
    xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    const bool used = i < s_history_count;
    if (used) {
      size_t start =
          (s_history_head + WEB_CONSOLE_HISTORY_SIZE - s_history_count) % WEB_CONSOLE_HISTORY_SIZE;
      size_t idx = (start + i) % WEB_CONSOLE_HISTORY_SIZE;
      doc["ts"] = (uint32_t)s_history[idx].ts;
      doc["msg"] = s_history[idx].msg;
    }
    xSemaphoreGive(s_history_mutex);
    if (!used) break;
    if (i) out.text(",");
    out.doc(doc);
  }
}
