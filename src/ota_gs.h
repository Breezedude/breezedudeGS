#pragma once

#include <Arduino.h>
#include "types.h"

#ifndef BREEZEDUDE_GS_ENABLE_LORA_OTA
  #define BREEZEDUDE_GS_ENABLE_LORA_OTA 1
#endif

void ota_gs_begin();
void ota_gs_try_update(const hwInfoData& info);
