#pragma once

#include <Arduino.h>
#include "types.h"

#ifndef BREEZEDUDE_GS_ENABLE_CONFIG_UPDATE
  #define BREEZEDUDE_GS_ENABLE_CONFIG_UPDATE 1
#endif

void config_gs_begin();
void config_gs_poll_devices();

// Immediately check and push pending config for a specific device (e.g. triggered from OTA path)
// nonce: device nonce from HWInfo announcement; used to validate the device ACK after sending
void config_gs_check_device(const String& deviceId, uint32_t nonce = 0u);
