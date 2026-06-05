 #pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include "esp_partition.h"

enum RadioModuleType : uint8_t {
  RADIO_MODULE_NONE = 0,
  RADIO_MODULE_SX1276,
  RADIO_MODULE_LLCC68,
  RADIO_MODULE_SX1262,
};

extern PhysicalLayer* radio_phy;
extern RadioModuleType lora_module;

extern SX1276 radio_sx1276;
extern LLCC68 radio_llcc68;
extern SX1262 radio_sx1262;

bool init_lora_radio();
void configure_lora_radio(bool fastMode);


inline uint16_t get_uuid(){
  return (uint16_t) (0xFFFF & ESP.getEfuseMac());
}

inline time_t getNtpTime() {
  time_t now = time(nullptr);
  return now;
}

inline size_t getLittleFSPartitionSize() {
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
inline int distance(float lat1, float lon1, float lat2, float lon2){
  // Haversine formula
  const double R = 6371; // Earth radius in km
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1);
  double a = sin(dLat/2) * sin(dLat/2) + cos(radians(lat1)) * cos(radians(lat2)) * sin(dLon/2) * sin(dLon/2);
  double c = 2 * atan2(sqrt(a), sqrt(1-a));
  return R * c;
}

