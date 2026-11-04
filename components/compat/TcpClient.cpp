#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Arduino.h"
#include "Client.h"
#include "esp_log.h"
#include "lwip/sockets.h"

static const char* TAG = "tcpclient";

static constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;

int WiFiClient::connect(const char* host, uint16_t port) {
  stop();
  if (!host || !*host) return 0;

  char service[8];
  snprintf(service, sizeof(service), "%u", (unsigned)port);

  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* result = nullptr;
  if (getaddrinfo(host, service, &hints, &result) != 0 || !result) {
    ESP_LOGW(TAG, "DNS lookup for %s failed", host);
    if (result) freeaddrinfo(result);
    return 0;
  }

  int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (sock < 0) {
    freeaddrinfo(result);
    return 0;
  }

  // Non-blocking connect so a dead APRS server cannot stall the task for the
  // full TCP timeout.
  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);

  int rc = ::connect(sock, result->ai_addr, result->ai_addrlen);
  freeaddrinfo(result);

  if (rc != 0 && errno == EINPROGRESS) {
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(sock, &write_set);
    struct timeval tv = {};
    tv.tv_sec = CONNECT_TIMEOUT_MS / 1000;
    tv.tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000;
    rc = select(sock + 1, nullptr, &write_set, nullptr, &tv);
    if (rc > 0) {
      int so_error = 0;
      socklen_t len = sizeof(so_error);
      getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
      rc = (so_error == 0) ? 0 : -1;
    } else {
      rc = -1;
    }
  }

  if (rc != 0) {
    close(sock);
    return 0;
  }

  int keepalive = 1;
  setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

  fd_ = sock;
  rx_len_ = rx_pos_ = 0;
  return 1;
}

int WiFiClient::connect(IPAddress ip, uint16_t port) { return connect(ip.toString().c_str(), port); }

void WiFiClient::stop() {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  rx_len_ = rx_pos_ = 0;
}

bool WiFiClient::fill() {
  if (fd_ < 0) return false;
  if (rx_pos_ < rx_len_) return true;

  int n = recv(fd_, rx_, sizeof(rx_), MSG_DONTWAIT);
  if (n > 0) {
    rx_len_ = (size_t)n;
    rx_pos_ = 0;
    return true;
  }
  if (n == 0) {
    // Orderly shutdown by the peer.
    stop();
    return false;
  }
  if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
    ESP_LOGD(TAG, "recv failed: %d", errno);
    stop();
  }
  return false;
}

uint8_t WiFiClient::connected() {
  if (fd_ < 0) return 0;
  if (rx_pos_ < rx_len_) return 1;
  fill();  // detects a closed peer and clears fd_
  return fd_ >= 0 ? 1 : 0;
}

int WiFiClient::available() {
  if (!fill()) return 0;
  return (int)(rx_len_ - rx_pos_);
}

int WiFiClient::read() {
  if (!fill()) return -1;
  return rx_[rx_pos_++];
}

int WiFiClient::read(uint8_t* buffer, size_t size) {
  if (!buffer || size == 0) return 0;
  if (!fill()) return -1;
  size_t pending = rx_len_ - rx_pos_;
  size_t count = size < pending ? size : pending;
  memcpy(buffer, &rx_[rx_pos_], count);
  rx_pos_ += count;
  return (int)count;
}

int WiFiClient::peek() {
  if (!fill()) return -1;
  return rx_[rx_pos_];
}

size_t WiFiClient::write(const uint8_t* buffer, size_t size) {
  if (fd_ < 0 || !buffer || size == 0) return 0;
  size_t sent = 0;
  while (sent < size) {
    int n = send(fd_, buffer + sent, size - sent, 0);
    if (n > 0) {
      sent += (size_t)n;
      continue;
    }
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)) {
      delay(5);
      continue;
    }
    ESP_LOGD(TAG, "send failed: %d", errno);
    stop();
    break;
  }
  return sent;
}

size_t WiFiClient::write(uint8_t c) { return write(&c, 1); }
