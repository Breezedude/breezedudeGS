// Serial console that writes to UART0 *and* the native USB-Serial-JTAG port, so
// one firmware image works on CP2102 boards (Heltec V3) and on boards where the
// USB-C socket goes straight to the SoC.
//
// Light sleep stays possible: the UART driver takes a PM lock only while it is
// actually transmitting, and CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION holds sleep off
// only while a USB host is attached.
#pragma once

#include <stdint.h>

#include "Print.h"

class HardwareSerial : public Stream {
 public:
  // port 0 -> UART0 only, port -1 -> UART0 + USB-Serial-JTAG
  explicit HardwareSerial(int port) : port_(port) {}

  void begin(unsigned long baud = 115200, uint32_t config = 0, int8_t rx_pin = -1, int8_t tx_pin = -1);
  void end();

  // Routes ESP_LOGx output through this port. Passing false silences the log
  // channel, which the serial provisioning protocol uses to keep its replies
  // free of interleaved debug lines.
  void setDebugOutput(bool enable);

  int available() override;
  int read() override;
  int peek() override;
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  void flush() override;

  operator bool() const { return true; }

 private:
  int port_;
  int peeked_ = -1;
};

extern HardwareSerial Serial;   // UART0 + USB-Serial-JTAG
extern HardwareSerial Serial0;  // UART0 only

// True while a USB host has the USB-Serial-JTAG port open. The power manager
// uses it to report why the station is not sleeping.
bool serial_usb_host_connected();
