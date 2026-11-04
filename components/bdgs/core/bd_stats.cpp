// Reception statistics and battery history, see bd_stats.h.
//
// Both histories are rings of fixed-length time slots. A ring only stores its
// newest slot number; advancing it to the current slot clears every slot that
// was skipped, so a gap (deep sleep, no packets) reads as zero, not as stale data.
//
// Slots are numbered by wall-clock time (epoch / slot length) once SNTP has set
// the clock. Before that - after a power cycle without internet - they count
// from boot; the first valid time then simply renames the newest slot.
#include "bd_stats.h"

#include <math.h>
#include <time.h>

#include "bd_json_out.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static constexpr uint32_t STATS_SLOT_S = 15 * 60;
static constexpr uint16_t STATS_SLOTS = 24 * 4;            // 24 h
static constexpr uint32_t BATTERY_SLOT_S = 15 * 60;
static constexpr uint16_t BATTERY_SLOTS = 3 * 24 * 4;      // 3 days
static constexpr uint32_t STATS_MAGIC = 0x42445354;         // "BDST"
static constexpr uint16_t STATS_LAYOUT = 2;
static constexpr time_t VALID_EPOCH = 1700000000;          // 2023-11

struct StatSlot {
  uint16_t rx[3];        // per BdStatProto
  uint16_t crcErr;
  uint16_t decodeFail;
  uint16_t fskErr;
  uint16_t snrN;
  uint16_t noiseN;
  int32_t snrSum;        // 0.1 dB
  int32_t rssiSum;       // dBm, over the same packets as snrSum
  int32_t noiseSum;      // dBm
  int16_t noiseMin;      // dBm
  int16_t wifiMin;       // dBm, STA signal
  int32_t wifiSum;       // dBm
  uint16_t wifiN;
  uint16_t pad;
};

struct BatterySlot {
  uint16_t minMv;
  uint16_t maxMv;
  uint16_t lastMv;
};

struct Ring {
  uint32_t headSlot;     // slot number of ring[headPos]
  uint16_t headPos;
  bool epochBased;
  bool used;
};

struct StatsStore {
  uint32_t magic;
  uint16_t layout;
  Ring statsRing;
  Ring batteryRing;
  StatSlot stats[STATS_SLOTS];
  BatterySlot battery[BATTERY_SLOTS];
};

// ~5.6 KB of RTC slow memory, untouched by software resets and deep sleep.
RTC_NOINIT_ATTR static StatsStore s_store;
static SemaphoreHandle_t s_mutex = nullptr;

static bool clock_valid() { return time(nullptr) > VALID_EPOCH; }

// Current slot number for a ring, and whether it is wall-clock based.
static uint32_t current_slot(uint32_t slotSeconds, bool& epochBased) {
  epochBased = clock_valid();
  if (epochBased) return (uint32_t)(time(nullptr) / slotSeconds);
  return (uint32_t)(millis() / 1000 / slotSeconds);
}

// Moves the ring head to the current slot, clearing every slot it passes.
// Returns the position to write to.
template <typename T>
static uint16_t advance(Ring& ring, T* slots, uint16_t count, uint32_t slotSeconds) {
  bool epochBased;
  uint32_t now = current_slot(slotSeconds, epochBased);

  if (!ring.used || (ring.epochBased && !epochBased)) {
    // First use, or the clock went from valid to unknown (power cycle while
    // the RTC memory survived): start over.
    memset(slots, 0, sizeof(T) * count);
    ring = {now, 0, epochBased, true};
    return 0;
  }
  if (!ring.epochBased && epochBased) {
    // SNTP just set the clock: the newest slot is "now" from here on.
    ring.headSlot = now;
    ring.epochBased = true;
    return ring.headPos;
  }
  if (now < ring.headSlot) {
    // Clock stepped backwards by more than a slot - keep writing to the head
    // rather than corrupting history.
    return ring.headPos;
  }

  uint32_t steps = now - ring.headSlot;
  if (steps >= count) {
    memset(slots, 0, sizeof(T) * count);
    ring.headPos = 0;
  } else {
    for (uint32_t i = 0; i < steps; i++) {
      ring.headPos = (uint16_t)((ring.headPos + 1) % count);
      memset(&slots[ring.headPos], 0, sizeof(T));
    }
  }
  ring.headSlot = now;
  return ring.headPos;
}

static StatSlot& stats_slot() {
  return s_store.stats[advance(s_store.statsRing, s_store.stats, STATS_SLOTS, STATS_SLOT_S)];
}

void bd_stats_init() {
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
  if (s_store.magic != STATS_MAGIC || s_store.layout != STATS_LAYOUT ||
      s_store.statsRing.headPos >= STATS_SLOTS || s_store.batteryRing.headPos >= BATTERY_SLOTS) {
    memset(&s_store, 0, sizeof(s_store));
    s_store.magic = STATS_MAGIC;
    s_store.layout = STATS_LAYOUT;
  }
}

static void lock() {
  if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock() {
  if (s_mutex) xSemaphoreGive(s_mutex);
}

static inline void bump(uint16_t& counter) {
  if (counter < UINT16_MAX) counter++;
}

void bd_stats_rx(BdStatProto proto, float snr, float rssi) {
  if (proto > BD_STAT_ADSL) return;
  lock();
  StatSlot& slot = stats_slot();
  bump(slot.rx[proto]);
  // SNR only means something for LoRa; FLARM and ADS-L contribute RSSI only
  // through their packet counts.
  if (proto == BD_STAT_FANET && !isnan(snr) && !isnan(rssi) && slot.snrN < UINT16_MAX) {
    slot.snrN++;
    slot.snrSum += (int32_t)lroundf(snr * 10.0f);
    slot.rssiSum += (int32_t)lroundf(rssi);
  }
  unlock();
}

void bd_stats_crc_error() {
  lock();
  bump(stats_slot().crcErr);
  unlock();
}

void bd_stats_decode_fail() {
  lock();
  bump(stats_slot().decodeFail);
  unlock();
}

void bd_stats_fsk_error() {
  lock();
  bump(stats_slot().fskErr);
  unlock();
}

void bd_stats_noise(float rssi) {
  if (isnan(rssi) || rssi > 0 || rssi < -160) return;
  lock();
  StatSlot& slot = stats_slot();
  int16_t value = (int16_t)lroundf(rssi);
  if (slot.noiseN == 0 || value < slot.noiseMin) slot.noiseMin = value;
  if (slot.noiseN < UINT16_MAX) {
    slot.noiseN++;
    slot.noiseSum += value;
  }
  unlock();
}

void bd_stats_wifi(int rssi) {
  if (rssi >= 0 || rssi < -127) return;
  lock();
  StatSlot& slot = stats_slot();
  if (slot.wifiN == 0 || rssi < slot.wifiMin) slot.wifiMin = (int16_t)rssi;
  if (slot.wifiN < UINT16_MAX) {
    slot.wifiN++;
    slot.wifiSum += rssi;
  }
  unlock();
}

void bd_stats_battery(float volts) {
  if (isnan(volts) || volts <= 0.5f || volts > 30.0f) return;
  uint16_t mv = (uint16_t)lroundf(volts * 1000.0f);
  lock();
  BatterySlot& slot = s_store.battery[advance(s_store.batteryRing, s_store.battery, BATTERY_SLOTS,
                                              BATTERY_SLOT_S)];
  if (slot.lastMv == 0 || mv < slot.minMv) slot.minMv = mv;
  if (slot.lastMv == 0 || mv > slot.maxMv) slot.maxMv = mv;
  slot.lastMv = mv;
  unlock();
}

// Layout (all arrays oldest slot first, the last entry is the current slot):
//   {"type":"stats","slot":900,"end":<epoch at the end of the newest slot, 0 if
//    the clock is unknown>,
//    "rx":[[fanet,flarm,adsl,crcErr,decodeFail,fskErr,snr10|null,rssi|null,
//           noiseMin|null,noiseAvg|null,wifiAvg|null,wifiMin|null], ...],
//    "bat":{"slot":900,"end":...,"mv":[[min,max,last]|0, ...]}}
// Slots without data read as zeros (rx) or 0 (bat).
void bd_stats_write(BdJsonOut& out, bool withBattery) {
  char buf[96];

  lock();
  stats_slot();  // bring the head up to date so the newest entry is "now"
  const Ring sr = s_store.statsRing;
  unlock();
  snprintf(buf, sizeof(buf), "{\"type\":\"stats\",\"slot\":%lu,\"end\":%lu,\"rx\":[",
           (unsigned long)STATS_SLOT_S,
           sr.epochBased ? (unsigned long)((sr.headSlot + 1) * STATS_SLOT_S) : 0UL);
  out.text(buf);
  for (uint16_t i = 0; i < STATS_SLOTS && out.ok(); i++) {
    lock();
    const StatSlot s = s_store.stats[(sr.headPos + 1 + i) % STATS_SLOTS];
    unlock();
    int n = snprintf(buf, sizeof(buf), "%s[%u,%u,%u,%u,%u,%u,", i ? "," : "", s.rx[0], s.rx[1],
                     s.rx[2], s.crcErr, s.decodeFail, s.fskErr);
    if (s.snrN) {
      n += snprintf(buf + n, sizeof(buf) - n, "%ld,%ld,", (long)(s.snrSum / s.snrN),
                    (long)(s.rssiSum / s.snrN));
    } else {
      n += snprintf(buf + n, sizeof(buf) - n, "null,null,");
    }
    if (s.noiseN) {
      n += snprintf(buf + n, sizeof(buf) - n, "%d,%ld,", s.noiseMin, (long)(s.noiseSum / s.noiseN));
    } else {
      n += snprintf(buf + n, sizeof(buf) - n, "null,null,");
    }
    if (s.wifiN) {
      snprintf(buf + n, sizeof(buf) - n, "%ld,%d]", (long)(s.wifiSum / s.wifiN), s.wifiMin);
    } else {
      snprintf(buf + n, sizeof(buf) - n, "null,null]");
    }
    out.text(buf);
  }
  out.text("]");

  lock();
  const bool haveBattery = withBattery && s_store.batteryRing.used;
  if (haveBattery) advance(s_store.batteryRing, s_store.battery, BATTERY_SLOTS, BATTERY_SLOT_S);
  const Ring br = s_store.batteryRing;
  unlock();
  if (haveBattery) {
    snprintf(buf, sizeof(buf), ",\"bat\":{\"slot\":%lu,\"end\":%lu,\"mv\":[",
             (unsigned long)BATTERY_SLOT_S,
             br.epochBased ? (unsigned long)((br.headSlot + 1) * BATTERY_SLOT_S) : 0UL);
    out.text(buf);
    for (uint16_t i = 0; i < BATTERY_SLOTS && out.ok(); i++) {
      lock();
      const BatterySlot b = s_store.battery[(br.headPos + 1 + i) % BATTERY_SLOTS];
      unlock();
      if (b.lastMv) {
        snprintf(buf, sizeof(buf), "%s[%u,%u,%u]", i ? "," : "", b.minMv, b.maxMv, b.lastMv);
      } else {
        snprintf(buf, sizeof(buf), "%s0", i ? "," : "");
      }
      out.text(buf);
    }
    out.text("]}");
  }
  out.text("}");
}
