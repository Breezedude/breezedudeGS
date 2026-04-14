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

// calculates distance in km between two lat/lon points using the Haversine formula
int distance(float lat1, float lon1, float lat2, float lon2){
  // Haversine formula
  const double R = 6371; // Earth radius in km
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1);
  double a = sin(dLat/2) * sin(dLat/2) + cos(radians(lat1)) * cos(radians(lat2)) * sin(dLon/2) * sin(dLon/2);
  double c = 2 * atan2(sqrt(a), sqrt(1-a));
  return R * c;
}

