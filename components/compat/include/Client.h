#pragma once

#include "IPAddress.h"
#include "Print.h"

class Client : public Stream {
 public:
  virtual int connect(const char* host, uint16_t port) = 0;
  virtual int connect(IPAddress ip, uint16_t port) = 0;
  virtual void stop() = 0;
  virtual uint8_t connected() = 0;
};

// Plain TCP client over lwIP BSD sockets. Keeps the Arduino semantics the APRS
// client relies on: available() never blocks, read() returns -1 when empty,
// print() pushes straight out, connected() stays true while the peer is up.
class WiFiClient : public Client {
 public:
  WiFiClient() = default;
  ~WiFiClient() override { stop(); }

  int connect(const char* host, uint16_t port) override;
  int connect(IPAddress ip, uint16_t port) override;
  void stop() override;
  uint8_t connected() override;

  int available() override;
  int read() override;
  int read(uint8_t* buffer, size_t size) override;
  int peek() override;
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  void flush() override {}

  int fd() const { return fd_; }

 private:
  bool fill();

  int fd_ = -1;
  uint8_t rx_[256] = {0};
  size_t rx_len_ = 0;
  size_t rx_pos_ = 0;
};
