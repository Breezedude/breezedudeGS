// Diagnostic reports to the Breezedude server and linking the station to an
// account in the device portal (devices.breezedude.de).
//
// The station identifies itself with its eFuse MAC and a random device key,
// created once and kept in NVS. The server registers the key with the first
// report and rejects any other afterwards.
#pragma once

#include <stdint.h>

#include "WString.h"

#ifndef GS_DIAG_URL
#define GS_DIAG_URL "https://fanet.breezedude.de/api/gs/report"
#endif
#ifndef GS_CLAIM_URL
#define GS_CLAIM_URL "https://fanet.breezedude.de/api/gs/claim-code"
#endif
#define GS_PORTAL_CLAIM_URL "https://devices.breezedude.de/gs-claim"

void bd_diag_begin();

// Called from the radio task for every FANET packet with a valid CRC.
void bd_diag_note_packet(uint8_t vid, uint16_t addr, uint8_t type, float rssi, float snr);
void bd_diag_note_flarm();
void bd_diag_note_adsl();
void bd_diag_note_upload(bool ok);
void bd_diag_note_queue_dropped();
void bd_diag_note_wifi_reconnect();

// From the net task: sends a report when one is due, and a pending link request.
void bd_diag_tick();

// A link request from websocket client `sockfd`; the answer is sent to it from
// the net task as {"diag_claim":{...}}.
void bd_diag_request_claim(int sockfd);

// For the status message: {"enabled","lastTs","lastCode","accepted","rejected"}.
struct BdDiagState {
  bool enabled;
  uint32_t lastTs;   // epoch of the last attempt, 0 if none
  int lastCode;      // HTTP status, negative for no response
  bool accepted;     // a report was taken at least once (also before this boot)
  bool rejected;     // the server refuses the device key (401)
};
BdDiagState bd_diag_state();
String bd_diag_station_id();
