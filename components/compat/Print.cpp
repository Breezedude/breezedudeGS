#include "Print.h"

#include <stdio.h>
#include <stdlib.h>

#include "Arduino.h"

size_t Print::write(const uint8_t* buffer, size_t size) {
  size_t written = 0;
  for (size_t i = 0; i < size; i++) {
    if (!write(buffer[i])) break;
    written++;
  }
  return written;
}

size_t Print::vprintf(const char* format, va_list args) {
  char stack_buf[160];
  va_list copy;
  va_copy(copy, args);
  int needed = vsnprintf(stack_buf, sizeof(stack_buf), format, copy);
  va_end(copy);
  if (needed < 0) return 0;

  if ((size_t)needed < sizeof(stack_buf)) {
    return write((const uint8_t*)stack_buf, (size_t)needed);
  }

  char* heap_buf = (char*)malloc((size_t)needed + 1);
  if (!heap_buf) return write((const uint8_t*)stack_buf, sizeof(stack_buf) - 1);
  vsnprintf(heap_buf, (size_t)needed + 1, format, args);
  size_t written = write((const uint8_t*)heap_buf, (size_t)needed);
  free(heap_buf);
  return written;
}

size_t Print::printf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  size_t written = vprintf(format, args);
  va_end(args);
  return written;
}

int Stream::read(uint8_t* buffer, size_t size) {
  size_t count = 0;
  while (count < size) {
    int c = read();
    if (c < 0) break;
    buffer[count++] = (uint8_t)c;
  }
  return (int)count;
}

size_t Stream::readBytes(uint8_t* buffer, size_t size) {
  size_t count = 0;
  uint32_t start = millis();
  while (count < size && (millis() - start) < timeout_ms_) {
    int c = read();
    if (c < 0) {
      delay(1);
      continue;
    }
    buffer[count++] = (uint8_t)c;
  }
  return count;
}
