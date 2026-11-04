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

// Closes packets whose copy window is over and reports them. Radio task only.
void bd_repeat_tick();

// Milliseconds until the next tick has work, UINT32_MAX without an open packet.
uint32_t bd_repeat_next_delay_ms();

// {"sources":[...],"events":[...]} - see bd_repeat.cpp.
void bd_repeat_write(BdJsonOut& out);

void bd_repeat_reset();
