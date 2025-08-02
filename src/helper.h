 #pragma once

#include <Arduino.h>
#include "esp_partition.h"


uint16_t get_uuid(){
  return (uint16_t) (0xFFFF & ESP.getEfuseMac());
}

time_t getNtpTime() {
  time_t now = time(nullptr);
  return now;
}

size_t getLittleFSPartitionSize() {
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS,// ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
        NULL
    );
    if (partition != NULL) {
        return partition->size;
    } else {
        Serial.println("LittleFS partition not found!");
        return 0;
    }
}

