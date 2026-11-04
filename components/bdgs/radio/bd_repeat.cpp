#include "bd_repeat.h"

#include <ArduinoJson.h>
#include <string.h>
#include <time.h>

#include "bd_json_out.h"
#include "bd_web.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "types.h"

// A repeater waits up to 1.5 s and may have to wait for a free channel on top,
// so a packet is reported after this time. A station keeps its own packets 4 s
// apart.
static constexpr uint32_t kCopyWindowMs = 3500;
// A copy can still come much later, from a repeater that sleeps through its
// forward time. It then upgrades the packet that was already reported.
static constexpr uint32_t kLateCopyMs = 60000;
static constexpr int kOpenPackets = 8;
static constexpr int kSources = 16;
static constexpr int kEvents = 120;
static constexpr size_t kNameLen = 20;

enum RepeatKind : uint8_t {
  KIND_BOTH = 0,      // original and repeated copy
  KIND_DIRECT = 1,    // forward bit set, no copy arrived
  KIND_REPEATED = 2,  // only the copy: the sender usually sets the bit, the original was not heard
  KIND_PLAIN = 3,     // no forward bit, and the sender was never seen with it for this type
};

struct OpenPacket {
  bool used;
  bool direct;
  uint8_t vid;
  uint8_t type;
  uint8_t copies;
  uint16_t addr;
  uint16_t delayMs;
  int8_t rssiDirect, rssiCopy;
  int16_t snrDirect, snrCopy;  // 0.1 dB
  uint32_t crc;
  uint32_t openedMs;
  uint32_t ts;
};

struct RepeatEvent {
  uint32_t seq;
  uint32_t crc;
  uint32_t openedMs;
  uint32_t ts;
  uint16_t addr;
  uint16_t delayMs;
  int16_t snrDirect, snrCopy;
  int8_t rssiDirect, rssiCopy;
  uint8_t vid;
  uint8_t type;
  uint8_t kind;
  uint8_t copies;
};

struct RepeatSource {
  bool used;
  uint8_t vid;
  uint16_t addr;
  uint16_t flagTypes;  // bit per packet type seen with the forward bit
  uint16_t minDelay, maxDelay;
  uint32_t counts[4];  // by RepeatKind
  uint32_t extraCopies;
  uint32_t nDirect, nCopy, nDelay;
  int32_t sumRssiDirect, sumSnrDirect, sumRssiCopy, sumSnrCopy;
  uint32_t sumDelay;
  uint32_t lastMs;
  uint32_t lastTs;
  char name[kNameLen + 1];  // from its name packets; a station without weather has no other
};

static OpenPacket s_open[kOpenPackets];
static RepeatSource s_sources[kSources];
static RepeatEvent s_events[kEvents];
static int s_event_head = 0;
static int s_event_count = 0;
static uint32_t s_event_seq = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// OTA and acknowledgement traffic never carries the forward bit and says
// nothing about a repeater.
static bool type_tracked(uint8_t type) {
  return type == FANET_PCK_TYPE_TRACKING || type == FANET_PCK_TYPE_NAME ||
         type == FANET_PCK_TYPE_WEATHER || type == FANET_PCK_TYPE_GROUND_TRACKING ||
         type == FANET_PCK_TYPE_HW_INFO;
}

static RepeatSource* source_for(uint8_t vid, uint16_t addr) {
  RepeatSource* oldest = &s_sources[0];
  for (int i = 0; i < kSources; i++) {
    RepeatSource& s = s_sources[i];
    if (s.used && s.vid == vid && s.addr == addr) return &s;
    if (!s.used) {
      oldest = &s;
    } else if (oldest->used && (int32_t)(s.lastMs - oldest->lastMs) < 0) {
      oldest = &s;
    }
  }
  memset(oldest, 0, sizeof(*oldest));
  oldest->used = true;
  oldest->vid = vid;
  oldest->addr = addr;
  return oldest;
}

static void event_json(JsonObject out, const RepeatEvent& e) {
  out["seq"] = e.seq;
  out["ts"] = e.ts;
  out["vid"] = e.vid;
  out["addr"] = e.addr;
  out["t"] = e.type;
  out["k"] = e.kind;
  out["n"] = e.copies;
  if (e.kind == KIND_BOTH || e.kind == KIND_DIRECT) {
    out["rd"] = e.rssiDirect;
    out["sd"] = e.snrDirect / 10.0;
  }
  if (e.copies > 0 && e.kind != KIND_PLAIN) {
    out["rc"] = e.rssiCopy;
    out["sc"] = e.snrCopy / 10.0;
  }
  if (e.kind == KIND_PLAIN) {
    out["rd"] = e.rssiCopy;
    out["sd"] = e.snrCopy / 10.0;
  }
  if (e.kind == KIND_BOTH) out["dl"] = e.delayMs;
}

static void source_json(JsonObject out, const RepeatSource& s) {
  out["vid"] = s.vid;
  out["addr"] = s.addr;
  out["both"] = s.counts[KIND_BOTH];
  out["direct"] = s.counts[KIND_DIRECT];
  out["repeated"] = s.counts[KIND_REPEATED];
  out["plain"] = s.counts[KIND_PLAIN];
  out["extra"] = s.extraCopies;
  out["ts"] = s.lastTs;
  if (s.name[0]) out["name"] = (const char*)s.name;
  if (s.nDirect) {
    out["rd"] = s.sumRssiDirect / (int32_t)s.nDirect;
    out["sd"] = s.sumSnrDirect / (int32_t)s.nDirect / 10.0;
  }
  if (s.nCopy) {
    out["rc"] = s.sumRssiCopy / (int32_t)s.nCopy;
    out["sc"] = s.sumSnrCopy / (int32_t)s.nCopy / 10.0;
  }
  if (s.nDelay) {
    out["dl"] = s.sumDelay / s.nDelay;
    out["dlMin"] = s.minDelay;
    out["dlMax"] = s.maxDelay;
  }
}

static void broadcast_live(const RepeatEvent& e, const RepeatSource& s) {
  JsonDocument doc;
  JsonObject live = doc["repeat"].to<JsonObject>();
  event_json(live["event"].to<JsonObject>(), e);
  source_json(live["source"].to<JsonObject>(), s);
  String out;
  serializeJson(doc, out);
  bd_ws_broadcast(out);
}

static void add_copy_to_source(RepeatSource* s, int8_t rssi, int16_t snr) {
  s->sumRssiCopy += rssi;
  s->sumSnrCopy += snr;
  s->nCopy++;
}

static void add_delay_to_source(RepeatSource* s, uint16_t delayMs) {
  if (!s->nDelay || delayMs < s->minDelay) s->minDelay = delayMs;
  if (!s->nDelay || delayMs > s->maxDelay) s->maxDelay = delayMs;
  s->sumDelay += delayMs;
  s->nDelay++;
}

// Turns an already reported "direct only" packet into "direct + repeated".
static bool upgrade_reported_packet(uint32_t crc, uint8_t vid, uint16_t addr, uint8_t type,
                                    int8_t rssi, int16_t snr, uint32_t now) {
  RepeatEvent e;
  RepeatSource copy;
  bool found = false;
  portENTER_CRITICAL(&s_mux);
  for (int i = 1; i <= s_event_count && !found; i++) {
    RepeatEvent& ev = s_events[(s_event_head - i + kEvents) % kEvents];
    if (now - ev.openedMs > kLateCopyMs) break;
    if (ev.crc != crc || ev.vid != vid || ev.addr != addr || ev.type != type) continue;
    if (ev.kind == KIND_BOTH) {
      // A further copy of a packet that already has one: a second repeater.
      if (ev.copies < 255) ev.copies++;
      RepeatSource* s = source_for(vid, addr);
      s->extraCopies++;
      e = ev;
      copy = *s;
      found = true;
    } else if (ev.kind == KIND_DIRECT) {
      ev.kind = KIND_BOTH;
      ev.copies = 1;
      ev.rssiCopy = rssi;
      ev.snrCopy = snr;
      ev.delayMs = (uint16_t)(now - ev.openedMs);
      RepeatSource* s = source_for(vid, addr);
      if (s->counts[KIND_DIRECT]) s->counts[KIND_DIRECT]--;
      s->counts[KIND_BOTH]++;
      add_copy_to_source(s, rssi, snr);
      add_delay_to_source(s, ev.delayMs);
      e = ev;
      copy = *s;
      found = true;
    }
  }
  portEXIT_CRITICAL(&s_mux);
  if (found) broadcast_live(e, copy);
  return found;
}

static void close_packet(OpenPacket& p) {
  RepeatEvent e = {};
  e.crc = p.crc;
  e.openedMs = p.openedMs;
  e.ts = p.ts;
  e.addr = p.addr;
  e.vid = p.vid;
  e.type = p.type;
  e.copies = p.copies;
  e.delayMs = p.delayMs;
  e.rssiDirect = p.rssiDirect;
  e.snrDirect = p.snrDirect;
  e.rssiCopy = p.rssiCopy;
  e.snrCopy = p.snrCopy;

  RepeatSource copy;
  portENTER_CRITICAL(&s_mux);
  RepeatSource* s = source_for(p.vid, p.addr);
  if (p.direct) s->flagTypes |= 1u << p.type;
  const bool flagged = s->flagTypes & (1u << p.type);
  e.kind = p.direct ? (p.copies ? KIND_BOTH : KIND_DIRECT) : (flagged ? KIND_REPEATED : KIND_PLAIN);

  s->counts[e.kind]++;
  s->lastMs = p.openedMs;
  s->lastTs = p.ts;
  if (p.copies > 1) s->extraCopies += p.copies - 1;
  if (p.direct) {
    s->sumRssiDirect += p.rssiDirect;
    s->sumSnrDirect += p.snrDirect;
    s->nDirect++;
  } else if (e.kind == KIND_PLAIN) {
    // Without the bit it is the sender's own transmission, not a copy.
    s->sumRssiDirect += p.rssiCopy;
    s->sumSnrDirect += p.snrCopy;
    s->nDirect++;
  }
  if (e.kind == KIND_BOTH || e.kind == KIND_REPEATED) add_copy_to_source(s, p.rssiCopy, p.snrCopy);
  if (e.kind == KIND_BOTH) add_delay_to_source(s, p.delayMs);
  e.seq = ++s_event_seq;
  s_events[s_event_head] = e;
  s_event_head = (s_event_head + 1) % kEvents;
  if (s_event_count < kEvents) s_event_count++;
  copy = *s;
  portEXIT_CRITICAL(&s_mux);

  p.used = false;
  broadcast_live(e, copy);
}

void bd_repeat_tick() {
  const uint32_t now = millis();
  for (OpenPacket& p : s_open) {
    if (p.used && now - p.openedMs >= kCopyWindowMs) close_packet(p);
  }
}

uint32_t bd_repeat_next_delay_ms() {
  const uint32_t now = millis();
  uint32_t next = UINT32_MAX;
  for (const OpenPacket& p : s_open) {
    if (!p.used) continue;
    const uint32_t age = now - p.openedMs;
    const uint32_t left = age >= kCopyWindowMs ? 0 : kCopyWindowMs - age;
    if (left < next) next = left;
  }
  return next;
}

static void note_name(uint8_t vid, uint16_t addr, const uint8_t* text, size_t len) {
  char name[kNameLen + 1] = {};
  for (size_t i = 0; i < len && i < kNameLen && text[i] >= 0x20 && text[i] < 0x7F; i++) {
    name[i] = (char)text[i];
  }
  if (!name[0]) return;
  portENTER_CRITICAL(&s_mux);
  memcpy(source_for(vid, addr)->name, name, sizeof(name));
  portEXIT_CRITICAL(&s_mux);
}

void bd_repeat_note(const uint8_t* frame, size_t len, float snr, float rssi) {
  if (len < 4) return;
  const uint8_t type = frame[0] & 0x3F;
  if (!type_tracked(type)) return;
  const bool forward = frame[0] & 0x40;
  const uint8_t vid = frame[1];
  const uint16_t addr = frame[2] | (frame[3] << 8);
  const uint32_t crc = esp_rom_crc32_le(0, frame + 1, len - 1);
  const int16_t snr10 = (int16_t)(snr * 10);
  if (type == FANET_PCK_TYPE_NAME && !(frame[0] & 0x80)) note_name(vid, addr, frame + 4, len - 4);

  bd_repeat_tick();
  const uint32_t now = millis();

  // A copy belongs to the newest open packet with the same content. One with
  // the forward bit is always a new transmission of the sender.
  if (!forward) {
    OpenPacket* match = nullptr;
    for (OpenPacket& p : s_open) {
      if (p.used && p.crc == crc && p.vid == vid && p.addr == addr && p.type == type &&
          (!match || (int32_t)(p.openedMs - match->openedMs) > 0)) {
        match = &p;
      }
    }
    if (match) {
      if (match->copies == 0) {
        match->rssiCopy = (int8_t)rssi;
        match->snrCopy = snr10;
        match->delayMs = (uint16_t)(now - match->openedMs);
      }
      if (match->copies < 255) match->copies++;
      return;
    }
    if (upgrade_reported_packet(crc, vid, addr, type, (int8_t)rssi, snr10, now)) return;
  }

  OpenPacket* slot = &s_open[0];
  for (OpenPacket& p : s_open) {
    if (!p.used) {
      slot = &p;
      break;
    }
    if ((int32_t)(p.openedMs - slot->openedMs) < 0) slot = &p;
  }
  if (slot->used) close_packet(*slot);

  *slot = {};
  slot->used = true;
  slot->direct = forward;
  slot->vid = vid;
  slot->addr = addr;
  slot->type = type;
  slot->crc = crc;
  slot->openedMs = now;
  slot->ts = (uint32_t)time(nullptr);
  if (forward) {
    slot->rssiDirect = (int8_t)rssi;
    slot->snrDirect = snr10;
  } else {
    slot->copies = 1;
    slot->rssiCopy = (int8_t)rssi;
    slot->snrCopy = snr10;
  }
}

void bd_repeat_write(BdJsonOut& out) {
  out.text("{\"sources\":[");
  bool first = true;
  for (int i = 0; i < kSources && out.ok(); i++) {
    RepeatSource s;
    portENTER_CRITICAL(&s_mux);
    s = s_sources[i];
    portEXIT_CRITICAL(&s_mux);
    if (!s.used) continue;
    JsonDocument doc;
    source_json(doc.to<JsonObject>(), s);
    if (!first) out.text(",");
    first = false;
    out.doc(doc);
  }

  out.text("],\"events\":[");
  int head, count;
  portENTER_CRITICAL(&s_mux);
  head = s_event_head;
  count = s_event_count;
  portEXIT_CRITICAL(&s_mux);
  for (int i = 0; i < count && out.ok(); i++) {
    RepeatEvent e;
    portENTER_CRITICAL(&s_mux);
    e = s_events[(head - count + i + kEvents) % kEvents];
    portEXIT_CRITICAL(&s_mux);
    JsonDocument doc;
    event_json(doc.to<JsonObject>(), e);
    if (i) out.text(",");
    out.doc(doc);
  }
  out.text("]}");
}

void bd_repeat_reset() {
  portENTER_CRITICAL(&s_mux);
  memset(s_sources, 0, sizeof(s_sources));
  s_event_head = 0;
  s_event_count = 0;
  portEXIT_CRITICAL(&s_mux);
}
