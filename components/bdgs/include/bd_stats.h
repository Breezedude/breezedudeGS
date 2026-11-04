// Reception statistics and battery history for the dashboard.
//
// Reception is counted in 15-minute slots over the last 24 hours, the battery
// voltage over the last 3 days. Both live in RTC memory, so they survive
// software resets and the scheduled deep sleep; only a power cycle clears them.
#pragma once

class BdJsonOut;

#include <Arduino.h>

enum BdStatProto : uint8_t {
  BD_STAT_FANET = 0,
  BD_STAT_FLARM = 1,
  BD_STAT_ADSL = 2,
};

void bd_stats_init();

// A packet that passed its CRC and was decoded.
void bd_stats_rx(BdStatProto proto, float snr, float rssi);
// FANET: the radio received a frame but its CRC failed.
void bd_stats_crc_error();
// FANET: CRC ok, but the payload could not be decoded.
void bd_stats_decode_fail();
// FLARM / ADS-L: a frame failed its software CRC or did not decode.
void bd_stats_fsk_error();
// Channel RSSI sampled while no packet is being received.
void bd_stats_noise(float rssi);
// WiFi STA signal, sampled while connected.
void bd_stats_wifi(int rssi);
void bd_stats_battery(float volts);

// {"type":"stats",...} - see bd_stats.cpp for the layout.
void bd_stats_write(BdJsonOut& out, bool withBattery);
