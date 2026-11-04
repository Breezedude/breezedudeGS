// Everything the station sends over the websocket goes through here.
//
// Frames sit in two fixed rings, each client has its own read position in them.
// The memory is reserved once and a broadcast is stored once, however many
// clients there are. Only one job for the HTTP server task, which owns the
// sockets, is ever queued: esp_http_server takes its jobs through a mailbox with
// six slots and silently loses what does not fit. That can still hit the one job,
// without an error, so a job that did not run in time counts as lost.
//
// When a ring is full the oldest frame goes. A client that had not received it
// yet is told {"resync":true} and fetches /api/snapshot, so nothing is lost
// without the client knowing.
#include <sys/select.h>
#include <sys/socket.h>

#include <atomic>

#include "Arduino.h"
#include "bd_state.h"
#include "bd_web.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "wsout";

// Powers of two: read and write positions run freely and are masked.
static constexpr uint32_t kEventBytes = 16 * 1024;
static constexpr uint32_t kReplyBytes = 4 * 1024;
static constexpr size_t kMaxFrame = 3072;
static constexpr int kMaxClients = 7;
static constexpr int kFramesPerPass = 8;
// A client that takes nothing for this long is gone, whatever TCP thinks.
static constexpr uint32_t kStallCloseMs = 30000;
static constexpr int kBroadcast = -1;
static constexpr uint32_t kHeapFloor = 16 * 1024;
static constexpr uint32_t kJobLostMs = 1000;

struct FrameHead {
  uint16_t len;
  int16_t fd;
};

struct Ring {
  uint8_t* buf;
  uint32_t size;
  uint32_t rd;
  uint32_t wr;
};

struct WsClient {
  bool used;
  bool resync;
  bool statusWanted;
  int fd;
  uint32_t pos[2];  // per ring
  uint32_t stalledSinceMs;
};

static uint8_t s_reply_buf[kReplyBytes];
static uint8_t s_event_buf[kEventBytes];
// Replies come first: a command must not wait behind a backlog of live data, and
// live data must not push out a reply.
static Ring s_rings[2] = {{s_reply_buf, kReplyBytes, 0, 0}, {s_event_buf, kEventBytes, 0, 0}};
static constexpr int kRingReply = 0;
static constexpr int kRingEvent = 1;

static WsClient s_clients[kMaxClients];
static SemaphoreHandle_t s_mutex = nullptr;
static httpd_handle_t s_server = nullptr;
static std::atomic<uint32_t> s_job_queued_ms{0};  // 0: none queued
static std::atomic<uint32_t> s_jobs_lost{0};
static uint32_t s_dropped = 0;
static uint32_t s_resyncs = 0;
static uint32_t s_oversize = 0;
static char s_frame[kMaxFrame + 1];  // HTTP server task only

static void lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(s_mutex); }

static void ring_put(Ring& r, uint32_t pos, const void* data, size_t len) {
  const uint32_t off = pos & (r.size - 1);
  const size_t first = len < r.size - off ? len : r.size - off;
  memcpy(r.buf + off, data, first);
  memcpy(r.buf, (const uint8_t*)data + first, len - first);
}

static void ring_get(const Ring& r, uint32_t pos, void* data, size_t len) {
  const uint32_t off = pos & (r.size - 1);
  const size_t first = len < r.size - off ? len : r.size - off;
  memcpy(data, r.buf + off, first);
  memcpy((uint8_t*)data + first, r.buf, len - first);
}

static void evict_oldest(int ring) {
  Ring& r = s_rings[ring];
  FrameHead head;
  ring_get(r, r.rd, &head, sizeof(head));
  const uint32_t next = r.rd + sizeof(head) + head.len;
  for (WsClient& c : s_clients) {
    if (!c.used || c.pos[ring] != r.rd) continue;
    c.pos[ring] = next;
    if (head.fd == kBroadcast || head.fd == c.fd) {
      c.resync = true;
      s_dropped++;
    }
  }
  r.rd = next;
}

static void ring_add(int ring, int fd, const char* data, size_t len) {
  Ring& r = s_rings[ring];
  const FrameHead head = {(uint16_t)len, (int16_t)fd};
  while (r.size - (r.wr - r.rd) < sizeof(head) + len) evict_oldest(ring);
  ring_put(r, r.wr, &head, sizeof(head));
  ring_put(r, r.wr + sizeof(head), data, len);
  r.wr += sizeof(head) + len;
}

static void schedule_job();

static WsClient* find_client(int fd) {
  for (WsClient& c : s_clients) {
    if (c.used && c.fd == fd) return &c;
  }
  return nullptr;
}

// The next frame for this client into s_frame, 0 if there is none.
static size_t take_frame(int fd) {
  size_t len = 0;
  bool status = false;
  lock();
  WsClient* c = find_client(fd);
  if (c && c->resync) {
    c->resync = false;
    s_resyncs++;
    len = strlcpy(s_frame, "{\"resync\":true}", sizeof(s_frame));
  } else if (c && c->statusWanted) {
    c->statusWanted = false;
    status = true;
  } else if (c) {
    for (int i = 0; i < 2 && !len; i++) {
      Ring& r = s_rings[i];
      while (c->pos[i] != r.wr && !len) {
        FrameHead head;
        ring_get(r, c->pos[i], &head, sizeof(head));
        if (head.fd == kBroadcast || head.fd == fd) {
          ring_get(r, c->pos[i] + sizeof(head), s_frame, head.len);
          len = head.len;
        }
        c->pos[i] += sizeof(head) + head.len;
      }
    }
  }
  unlock();
  // Built when it goes out, so a client that lags behind gets the current state
  // instead of a backlog of old ones.
  if (status) len = bd_ws_status_json(s_frame, sizeof(s_frame));
  return len;
}

static bool has_frames(const WsClient& c) {
  return c.resync || c.statusWanted || c.pos[0] != s_rings[0].wr || c.pos[1] != s_rings[1].wr;
}

static void remove_client_locked(WsClient& c) {
  c.used = false;
  bool any = false;
  for (const WsClient& other : s_clients) any = any || other.used;
  if (!any) {
    // Verbose modes are per session and have to be switched on again deliberately.
    ota_verbose_ws = false;
    flarm_verbose_ws = false;
    adsl_verbose_ws = false;
  }
}

static void close_client(int fd) {
  lock();
  WsClient* c = find_client(fd);
  if (c) remove_client_locked(*c);
  unlock();
  httpd_sess_trigger_close(s_server, fd);
}

// A full send buffer would block the whole server until the send timeout.
static bool socket_writable(int fd) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(fd, &set);
  struct timeval none = {0, 0};
  return select(fd + 1, nullptr, &set, nullptr, &none) > 0;
}

static void send_pass(void*) {
  s_job_queued_ms.store(0);

  int fds[kMaxClients];
  int count = 0;
  lock();
  for (const WsClient& c : s_clients) {
    if (c.used && has_frames(c)) fds[count++] = c.fd;
  }
  unlock();

  bool more = false;
  for (int i = 0; i < count; i++) {
    const int fd = fds[i];
    bool stalled = !socket_writable(fd);
    if (stalled) {
      bool give_up = false;
      lock();
      WsClient* c = find_client(fd);
      if (c) {
        if (!c->stalledSinceMs) c->stalledSinceMs = millis();
        give_up = millis() - c->stalledSinceMs > kStallCloseMs;
      }
      unlock();
      if (give_up) {
        ESP_LOGW(TAG, "client %d took nothing for %lu s, closing", fd, (unsigned long)(kStallCloseMs / 1000));
        close_client(fd);
      }
      continue;
    }

    bool failed = false;
    for (int n = 0; n < kFramesPerPass && !stalled; n++) {
      const size_t len = take_frame(fd);
      if (!len) break;
      httpd_ws_frame_t frame = {};
      frame.type = HTTPD_WS_TYPE_TEXT;
      frame.payload = (uint8_t*)s_frame;
      frame.len = len;
      if (httpd_ws_send_frame_async(s_server, fd, &frame) != ESP_OK) {
        failed = true;
        break;
      }
      stalled = !socket_writable(fd);
    }
    if (failed) {
      close_client(fd);
      continue;
    }

    lock();
    WsClient* c = find_client(fd);
    if (c) {
      c->stalledSinceMs = stalled ? c->stalledSinceMs : 0;
      if (!stalled && has_frames(*c)) more = true;
    }
    unlock();
  }
  if (more) schedule_job();
}

static void schedule_job() {
  if (!s_server) return;
  const uint32_t now = millis() | 1;
  const uint32_t queued = s_job_queued_ms.load();
  if (queued) {
    if (now - queued < kJobLostMs) return;
    s_jobs_lost++;
  }
  s_job_queued_ms.store(now);
  if (httpd_queue_work(s_server, send_pass, nullptr) != ESP_OK) s_job_queued_ms.store(0);
}

void bd_ws_out_begin(httpd_handle_t server) {
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
  s_server = server;
}

void bd_ws_out_add_client(int fd) {
  lock();
  WsClient* c = find_client(fd);
  for (WsClient& slot : s_clients) {
    if (!c && !slot.used) c = &slot;
  }
  if (c) {
    *c = {};
    c->used = true;
    c->fd = fd;
    c->pos[0] = s_rings[0].wr;
    c->pos[1] = s_rings[1].wr;
  }
  unlock();
  if (!c) ESP_LOGW(TAG, "no room for websocket client %d", fd);
}

void bd_ws_out_remove_client(int fd) {
  if (!s_mutex) return;
  lock();
  WsClient* c = find_client(fd);
  if (c) remove_client_locked(*c);
  unlock();
}

void bd_ws_out_flush() {
  send_pass(nullptr);
}

static bool frame_fits(const String& text) {
  if (text.length() <= kMaxFrame) return true;
  ESP_LOGW(TAG, "frame of %u bytes dropped", (unsigned)text.length());
  lock();
  s_oversize++;
  unlock();
  return false;
}

void bd_ws_broadcast(const String& text) {
  if (text.isEmpty() || !bd_ws_has_clients() || !frame_fits(text)) return;
  lock();
  ring_add(kRingEvent, kBroadcast, text.c_str(), text.length());
  unlock();
  schedule_job();
}

void bd_ws_send(int fd, const String& text) {
  if (text.isEmpty() || !frame_fits(text)) return;
  lock();
  if (find_client(fd)) ring_add(kRingReply, fd, text.c_str(), text.length());
  unlock();
  schedule_job();
}

void bd_ws_request_status(int fd) {
  lock();
  WsClient* c = find_client(fd);
  if (c) c->statusWanted = true;
  unlock();
  schedule_job();
}

bool bd_ws_has_clients() { return bd_ws_client_count() > 0; }

int bd_ws_client_count() {
  if (!s_mutex) return 0;
  int count = 0;
  lock();
  for (const WsClient& c : s_clients) count += c.used ? 1 : 0;
  unlock();
  return count;
}

bool bd_ws_heap_low() { return esp_get_free_heap_size() < kHeapFloor; }

void bd_ws_note_dropped() {
  if (!s_mutex) return;
  lock();
  s_dropped++;
  for (WsClient& c : s_clients) {
    if (c.used) c.resync = true;
  }
  unlock();
  schedule_job();
}

BdWsOutStats bd_ws_out_stats() {
  BdWsOutStats out = {};
  if (!s_mutex) return out;
  lock();
  for (const WsClient& c : s_clients) out.clients += c.used ? 1 : 0;
  out.dropped = s_dropped;
  out.resyncs = s_resyncs;
  out.oversize = s_oversize;
  out.jobsLost = s_jobs_lost.load();
  unlock();
  return out;
}
