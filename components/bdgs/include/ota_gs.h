#pragma once

class BdJsonOut;

#include <Arduino.h>
#include "types.h"

#ifndef BREEZEDUDE_GS_ENABLE_LORA_OTA
  #define BREEZEDUDE_GS_ENABLE_LORA_OTA 1
#endif

void ota_gs_begin();
void ota_gs_try_update(const hwInfoData& info);
// Right after a HW info packet was decoded: asks a sensor for its OTA
// announcement when an update for it is ready here. True if it did.
bool ota_gs_hold(const hwInfoData& info);

// Every HW info packet of a sensor: learns when its next OTA announcement is
// due (type 2 comes hourly, the first 10 minutes after boot) and marks the
// sensor for a firmware preload. Radio task.
void ota_gs_note_hwinfo(const hwInfoData& info);

// Asks the backend which image the sensor's next update will need and puts it
// into the flash store, so the update starts as soon as the sensor announces
// itself instead of after a download it does not wait for. Net task.
void ota_gs_preload_tick();

// The last update decisions per sensor ("up_to_date", "start_not_acked", ...)
// as JSON array items {"id":"BD793C","entries":[...]},..., oldest entry first.
void ota_gs_history_write(BdJsonOut& out);

// A sensor update is running; the radio must not be disturbed meanwhile.
bool ota_gs_busy();
