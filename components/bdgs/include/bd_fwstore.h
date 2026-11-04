// Firmware images for sensors, kept on flash so a sensor update neither needs
// the image in RAM nor the WiFi while the radio transfer runs. An image stays
// until the space is needed for another one, so the next sensor wanting the
// same version gets it without a download.
//
// One caller at a time: the sensor update in the radio task.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_partition.h"

struct BdFwImage {
  const esp_partition_t* part = nullptr;
  uint32_t offset = 0;  // of the image data inside the partition
  uint32_t size = 0;
};

// An image with this SHA-256, checked against the flash contents.
bool bd_fwstore_find(const uint8_t sha256[32], uint32_t size, BdFwImage* out);

// Storing a new one: begin() makes room (dropping the image unused the longest)
// and erases it, write() appends, finish() checks the SHA-256 and only then
// makes the image findable. An image that is not finished is never used.
struct BdFwWriter {
  BdFwImage image;
  uint32_t written = 0;
  bool ok = false;
};
bool bd_fwstore_begin(uint32_t size, BdFwWriter* w);
bool bd_fwstore_write(BdFwWriter* w, const uint8_t* data, size_t len);
bool bd_fwstore_finish(BdFwWriter* w, const uint8_t sha256[32], const char* name, BdFwImage* out);

bool bd_fwstore_read(const BdFwImage& image, uint32_t offset, void* buf, size_t len);

// Total and free bytes over all store partitions, 0 if there is none.
void bd_fwstore_space(uint32_t* total, uint32_t* free);
