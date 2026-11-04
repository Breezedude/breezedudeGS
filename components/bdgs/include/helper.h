#pragma once

#include <Arduino.h>
#include <RadioLib.h>

#include "esp_partition.h"

enum RadioModuleType : uint8_t {
  RADIO_MODULE_NONE = 0,
  RADIO_MODULE_SX1276,
  RADIO_MODULE_LLCC68,
  RADIO_MODULE_SX1262,
};

// Which protocol the single transceiver is currently tuned to. FANET uses LoRa,
// FLARM 2-GFSK on two channels, so only one can be received at a time.
enum RadioMode : uint8_t {
  RADIO_MODE_FANET = 0,
  RADIO_MODE_FLARM_LOW,   // FLARM, 868.2 MHz
  RADIO_MODE_FLARM_HIGH,  // FLARM, 868.4 MHz
  RADIO_MODE_ADSL_LOW,    // ADS-L, 868.2 MHz
  RADIO_MODE_ADSL_HIGH,   // ADS-L, 868.4 MHz
};

// Packet-pending flag for the DIO1 interrupt.
//
// On ESP-IDF the IRQ line doubles as the light-sleep wake source, and GPIO wake
// only works with level triggers (gpio_wakeup_enable refuses edges). A level
// trigger would re-enter the ISR forever, so the ISR disables the interrupt and
// it is re-armed when a consumer clears the flag - which every receive path
// already does ("receivedFlag = false") before reading the packet out.
class RadioIrqFlag {
 public:
  operator bool();
  RadioIrqFlag& operator=(bool value);

  // Re-arms immediately; used by the radio task before it blocks.
  void arm();
  void setFromIsr() { flag_ = true; }

 private:
  volatile bool flag_ = false;
  volatile bool rearm_pending_ = false;
};

extern RadioIrqFlag receivedFlag;

extern PhysicalLayer* radio_phy;
extern RadioModuleType lora_module;
extern RadioMode radio_mode;

extern SX1276 radio_sx1276;
extern LLCC68 radio_llcc68;
extern SX1262 radio_sx1262;

bool init_lora_radio();
void configure_lora_radio(bool fastMode);

// Drives the FANET/FLARM/ADS-L time sharing; called from the radio task.
void flarm_scheduler_tick();
// Milliseconds until the scheduler needs attention again. UINT32_MAX means
// "only a packet matters", which is what lets the CPU sleep through idle time.
uint32_t flarm_scheduler_next_delay_ms();
bool flarm_enabled();
bool adsl_enabled();
bool fanet_enabled();

extern uint32_t flarm_rx_frames;
extern uint32_t flarm_crc_errors;
extern uint32_t flarm_decode_fail;
extern uint32_t flarm_decoded;
extern uint32_t adsl_rx_frames;
extern uint32_t adsl_crc_errors;
extern uint32_t adsl_decode_fail;
extern uint32_t adsl_decoded;
extern uint32_t flarm_switch_us_max;
extern uint32_t flarm_entry_late_ms_max;

inline uint16_t get_uuid() { return (uint16_t)(0xFFFF & ESP.getEfuseMac()); }

inline time_t getNtpTime() {
  time_t now = time(nullptr);
  return now;
}

inline size_t getLittleFSPartitionSize() {
  const esp_partition_t* partition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "www");
  return partition != nullptr ? partition->size : 0;
}

// calculates distance in km between two lat/lon points using the Haversine formula
inline int distance(float lat1, float lon1, float lat2, float lon2) {
  const double R = 6371;  // Earth radius in km
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1);
  double a = sin(dLat / 2) * sin(dLat / 2) +
             cos(radians(lat1)) * cos(radians(lat2)) * sin(dLon / 2) * sin(dLon / 2);
  double c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return R * c;
}
