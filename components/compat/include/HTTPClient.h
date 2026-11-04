// Arduino HTTPClient facade over esp_http_client.
//
// The client object handed to begin() is ignored: TLS is decided by the URL
// scheme and handled by esp-tls (certificate bundle). getStreamPtr() returns a
// WiFiClient-typed reader so the firmware download loop in ota_gs.cpp keeps
// working unchanged.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "Client.h"
#include "WString.h"
#include "esp_http_client.h"

#define HTTP_CODE_OK 200
#define HTTP_CODE_CREATED 201
#define HTTP_CODE_ACCEPTED 202
#define HTTP_CODE_NO_CONTENT 204
#define HTTP_CODE_NOT_MODIFIED 304
#define HTTP_CODE_BAD_REQUEST 400
#define HTTP_CODE_NOT_FOUND 404
#define HTTP_CODE_INTERNAL_SERVER_ERROR 500

typedef enum {
  HTTPC_DISABLE_FOLLOW_REDIRECTS,
  HTTPC_STRICT_FOLLOW_REDIRECTS,
  HTTPC_FORCE_FOLLOW_REDIRECTS
} followRedirects_t;

class HttpBodyStream : public WiFiClient {
 public:
  void attach(esp_http_client_handle_t handle, int64_t content_length);
  void detach();

  int available() override;
  int read() override;
  int read(uint8_t* buffer, size_t size) override;
  int peek() override;
  uint8_t connected() override;
  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t*, size_t) override { return 0; }

 private:
  esp_http_client_handle_t handle_ = nullptr;
  int64_t content_length_ = -1;
  int64_t received_ = 0;
};

class HTTPClient {
 public:
  HTTPClient() = default;
  ~HTTPClient() { end(); }

  bool begin(const String& url);
  bool begin(WiFiClient& client, const String& url) { return begin(url); }
  bool begin(const String& host, uint16_t port, const String& path);
  void end();

  void addHeader(const String& name, const String& value);
  void setTimeout(uint16_t timeout_ms) { timeout_ms_ = timeout_ms; }
  void setConnectTimeout(int32_t timeout_ms) { timeout_ms_ = (uint16_t)timeout_ms; }
  void setFollowRedirects(followRedirects_t mode) { follow_ = mode; }
  void setReuse(bool) {}
  void setUserAgent(const String& agent) { user_agent_ = agent; }

  int GET();
  int POST(const String& payload);
  int POST(const uint8_t* payload, size_t size);
  int POST(uint8_t* payload, size_t size) { return POST((const uint8_t*)payload, size); }

  int getSize() const { return (int)content_length_; }
  String getString();
  String header(const char* name);
  WiFiClient* getStreamPtr() { return &stream_; }

 private:
  int request(esp_http_client_method_t method, const uint8_t* body, size_t body_len);
  void closeHandle();

  String url_;
  String user_agent_ = "breezedude-gs";
  std::vector<std::pair<String, String>> headers_;
  esp_http_client_handle_t handle_ = nullptr;
  HttpBodyStream stream_;
  int64_t content_length_ = -1;
  uint16_t timeout_ms_ = 10000;
  followRedirects_t follow_ = HTTPC_DISABLE_FOLLOW_REDIRECTS;
};
