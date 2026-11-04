// GET /api/snapshot: the station's whole state in one JSON document, see
// docs/web-api.md. Streamed entry by entry through a fixed buffer, so it costs the
// same RAM however full the stores are, and TCP paces it.
#include <ArduinoJson.h>
#include <string.h>
#include <time.h>

#include "bd_console.h"
#include "bd_json_out.h"
#include "bd_power.h"
#include "bd_repeat.h"
#include "bd_state.h"
#include "bd_stats.h"
#include "bd_web.h"
#include "esp_http_server.h"
#include "ota_gs.h"

static bool send_chunk(void* ctx, const char* data, size_t len) {
  return httpd_resp_send_chunk((httpd_req_t*)ctx, data, len) == ESP_OK;
}

// Serializing happens outside the store lock: it can wait on the socket.
template <typename Fill>
static void stream_entries(BdJsonOut& out, int count, Fill fill) {
  bool first = true;
  for (int i = 0; i < count && out.ok(); i++) {
    JsonDocument doc;
    store_lock();
    const bool used = fill(i, doc.to<JsonObject>());
    store_unlock();
    if (!used) continue;
    if (!first) out.text(",");
    first = false;
    out.doc(doc);
  }
}

static void write_stores(BdJsonOut& out, const char* parts, time_t now, long maxAgeS) {
  auto wanted = [parts](const char* part) { return !parts[0] || strstr(parts, part); };

  if (wanted("weather")) {
    out.text(",\"weather\":[");
    stream_entries(out, MAX_DEVICES, [now, maxAgeS](int i, JsonObject o) {
      if (!weatherStore[i].timestamp || now - weatherStore[i].timestamp >= maxAgeS) return false;
      fillWeather(o, &weatherStore[i]);
      return true;
    });
    out.text("]");
  }
  if (wanted("tracking")) {
    out.text(",\"tracking\":[");
    stream_entries(out, MAX_DEVICES, [now, maxAgeS](int i, JsonObject o) {
      if (!trackingStore[i].timestamp || now - trackingStore[i].timestamp >= maxAgeS) return false;
      fillTracking(o, &trackingStore[i]);
      return true;
    });
    out.text("]");
  }
  if (wanted("hwinfo")) {
    // Oldest first per station, as they arrived.
    out.text(",\"hwinfo\":[");
    stream_entries(out, HWINFO_MAX_STATIONS * HWINFO_HISTORY_PER_STATION, [](int i, JsonObject o) {
      const hwInfoData* hi = getHwInfoHistoryEntry(i / HWINFO_HISTORY_PER_STATION, i % HWINFO_HISTORY_PER_STATION);
      if (!hi) return false;
      fillHwInfo(o, hi);
      return true;
    });
    out.text("]");
  }
  if (wanted("ota")) {
    out.text(",\"ota_history\":[");
    ota_gs_history_write(out);
    out.text("]");
  }
  if (wanted("console")) {
    out.text(",\"console\":[");
    webconsole_history_write(out);
    out.text("]");
  }
  if (wanted("repeat")) {
    out.text(",\"repeat\":");
    bd_repeat_write(out);
  }
  if (wanted("stats")) {
    out.text(",\"stats\":");
    bd_stats_write(out, settings.batteryPowered);
  }
}

esp_err_t bd_snapshot_handler(httpd_req_t* req) {
  if (bd_ws_heap_low()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_hdr(req, "Retry-After", "5");
    httpd_resp_sendstr(req, "low on memory");
    return ESP_OK;
  }

  char query[96] = {};
  char parts[64] = {};
  char minutes[8] = {};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "parts", parts, sizeof(parts));
    httpd_query_key_value(query, "min", minutes, sizeof(minutes));
  }
  const long maxAgeS = (minutes[0] ? atol(minutes) : 120) * 60;

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  bd_power_hold_awake(true);

  const time_t now = time(nullptr);
  BdJsonOut out(send_chunk, req);
  char head[48];
  snprintf(head, sizeof(head), "{\"proto\":%d,\"ts\":%lu", BD_WS_PROTO, (unsigned long)now);
  out.text(head);
  write_stores(out, parts, now, maxAgeS);
  // Only a document that got this far is whole.
  out.text(",\"complete\":true}");
  const bool sent = out.flush() && httpd_resp_send_chunk(req, nullptr, 0) == ESP_OK;

  bd_power_hold_awake(false);
  return sent ? ESP_OK : ESP_FAIL;
}
