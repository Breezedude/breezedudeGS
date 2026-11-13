// Tells which FANET packets reached this station directly, through a repeater,
// or both.
//
// A sender that wants to be repeated sets the forward bit; a repeater clears it
// and sends the packet again, otherwise unchanged. The source address stays the
// sender's, so a packet does not say who repeated it. Copies are matched on
// sender, type and content, up to a minute after the original.
#pragma once

#include <Arduino.h>

class BdJsonOut;

// A FANET frame that passed its CRC. Radio task only.
void bd_repeat_note(const uint8_t* frame, size_t len, float snr, float rssi);

// Packet types whose header address is the sender's own: tracking, name,
// weather, ground tracking and HW info. Not an ACK, which carries the address
// of the station it answers.
bool bd_repeat_type_tracked(uint8_t type);

// Closes packets whose copy window is over and reports them. Radio task only.
void bd_repeat_tick();

// Milliseconds until the next tick has work, UINT32_MAX without an open packet.
uint32_t bd_repeat_next_delay_ms();

// {"sources":[...],"events":[...]} - see bd_repeat.cpp.
void bd_repeat_write(BdJsonOut& out);

void bd_repeat_reset();

// One received packet for the packet history in the device portal: when, from
// whom and how it came in - never its content.
struct BdPacketRecord {
  uint32_t seq;    // counts from boot
  uint32_t ageMs;  // how long ago it was received
  uint8_t vid;
  uint16_t addr;
  uint8_t type;
  uint8_t kind;    // 0 direct + repeated, 1 direct, 2 repeated only, 3 no forward bit
  int8_t rssiDirect, rssiCopy;
  int16_t snrDirect, snrCopy;  // 0.1 dB
  uint16_t delayMs;            // repeater delay, kind 0 only
};

// Packets after afterSeq that are at least minAgeMs old, oldest first. A packet
// can still gain a late repeated copy for a minute, so only older ones are final.
size_t bd_repeat_records_after(uint32_t afterSeq, uint32_t minAgeMs, BdPacketRecord* out, size_t max);
