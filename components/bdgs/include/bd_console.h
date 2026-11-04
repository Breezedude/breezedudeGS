// Log fan-out to the web UI.
//
// Same JSON messages the Arduino build sent ("webconsole", "aprsconsole",
// "ota_progress"), so www/app.js needs no changes.
#pragma once

class BdJsonOut;

#include <Arduino.h>

// Adds the line to the ring buffer a freshly connected client gets replayed.
void webconsole_print(String in);

// Live-only messages (no history): per-packet noise would push everything else
// out of the 20 line buffer.
void ota_status_v(const String& msg);
void aprsconsole_print(String in);
void flarm_debug_print(const String& msg);

void ota_progress_start(uint32_t totalChunks);
void ota_progress_chunk(uint16_t seq, uint8_t retries, float snr, uint8_t ackResendCount,
                        int8_t deviceSnr, int8_t deviceRssi, uint8_t proactiveAckResendCount);
void ota_progress_end(bool success);

// The history as JSON array items: {"ts":...,"msg":...},...
void webconsole_history_write(BdJsonOut& out);

String sanitizeLogLine(const String& input);
