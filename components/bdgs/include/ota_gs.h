#pragma once

class BdJsonOut;

#include <Arduino.h>
#include "types.h"

#ifndef BREEZEDUDE_GS_ENABLE_LORA_OTA
  #define BREEZEDUDE_GS_ENABLE_LORA_OTA 1
#endif

void ota_gs_begin();
void ota_gs_try_update(const hwInfoData& info);

// The last update decisions per sensor ("up_to_date", "start_not_acked", ...)
// as JSON array items {"id":"BD793C","entries":[...]},..., oldest entry first.
void ota_gs_history_write(BdJsonOut& out);

// A sensor update is running; the radio must not be disturbed meanwhile.
bool ota_gs_busy();
