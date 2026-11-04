#include "HTTPClient.h"

#include "Arduino.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char* TAG = "httpclient";

static constexpr int MAX_REDIRECTS = 5;

// --- body reader ------------------------------------------------------------
void HttpBodyStream::attach(esp_http_client_handle_t handle, int64_t content_length) {
  handle_ = handle;
  content_length_ = content_length;
  received_ = 0;
}

void HttpBodyStream::detach() {
  handle_ = nullptr;
  content_length_ = -1;
  received_ = 0;
}

int HttpBodyStream::available() {
  if (!handle_) return 0;
  if (content_length_ >= 0 && received_ >= content_length_) return 0;
  if (esp_http_client_is_complete_data_received(handle_)) return 0;
  // esp_http_client has no "bytes buffered" query; report one readable byte so
  // the caller proceeds to read(), which blocks only up to the HTTP timeout.
  return 1;
}

int HttpBodyStream::read(uint8_t* buffer, size_t size) {
  if (!handle_ || !buffer || size == 0) return -1;
  if (content_length_ >= 0) {
    int64_t remaining = content_length_ - received_;
    if (remaining <= 0) return -1;
    if ((int64_t)size > remaining) size = (size_t)remaining;
  }
  int got = esp_http_client_read(handle_, (char*)buffer, (int)size);
  if (got > 0) received_ += got;
  return got;
}

int HttpBodyStream::read() {
  uint8_t byte = 0;
  return read(&byte, 1) == 1 ? byte : -1;
}

int HttpBodyStream::peek() { return -1; }

uint8_t HttpBodyStream::connected() { return handle_ != nullptr ? 1 : 0; }

// --- client -----------------------------------------------------------------
bool HTTPClient::begin(const String& url) {
  end();
  if (url.isEmpty()) return false;
  url_ = url;
  return true;
}

bool HTTPClient::begin(const String& host, uint16_t port, const String& path) {
  String url = "http://" + host + ":" + String((unsigned long)port) + path;
  return begin(url);
}

void HTTPClient::closeHandle() {
  if (handle_) {
    esp_http_client_close(handle_);
    esp_http_client_cleanup(handle_);
    handle_ = nullptr;
  }
  stream_.detach();
}

void HTTPClient::end() {
  closeHandle();
  headers_.clear();
  content_length_ = -1;
}

void HTTPClient::addHeader(const String& name, const String& value) {
  headers_.emplace_back(name, value);
}

int HTTPClient::request(esp_http_client_method_t method, const uint8_t* body, size_t body_len) {
  closeHandle();
  content_length_ = -1;

  esp_http_client_config_t cfg = {};
  cfg.url = url_.c_str();
  cfg.method = method;
  cfg.timeout_ms = timeout_ms_;
  cfg.disable_auto_redirect = true;  // handled below so POST bodies can be replayed
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 1024;
  cfg.keep_alive_enable = false;
  cfg.skip_cert_common_name_check = false;
  if (url_.startsWith("https://")) {
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
  }

  handle_ = esp_http_client_init(&cfg);
  if (!handle_) {
    ESP_LOGW(TAG, "esp_http_client_init failed for %s", url_.c_str());
    return -1;
  }

  esp_http_client_set_header(handle_, "User-Agent", user_agent_.c_str());
  for (const auto& header : headers_) {
    esp_http_client_set_header(handle_, header.first.c_str(), header.second.c_str());
  }

  for (int redirect = 0;; redirect++) {
    esp_err_t err = esp_http_client_open(handle_, (int)body_len);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "open(%s) failed: %s", url_.c_str(), esp_err_to_name(err));
      closeHandle();
      return -1;
    }

    if (body && body_len > 0) {
      int written = esp_http_client_write(handle_, (const char*)body, (int)body_len);
      if (written < 0 || (size_t)written != body_len) {
        ESP_LOGW(TAG, "write body failed (%d of %u)", written, (unsigned)body_len);
        closeHandle();
        return -1;
      }
    }

    content_length_ = esp_http_client_fetch_headers(handle_);
    int status = esp_http_client_get_status_code(handle_);

    const bool is_redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    if (is_redirect && follow_ != HTTPC_DISABLE_FOLLOW_REDIRECTS && redirect < MAX_REDIRECTS) {
      if (esp_http_client_set_redirection(handle_) == ESP_OK) {
        esp_http_client_close(handle_);
        continue;
      }
    }

    stream_.attach(handle_, content_length_);
    return status;
  }
}

int HTTPClient::GET() { return request(HTTP_METHOD_GET, nullptr, 0); }

int HTTPClient::POST(const String& payload) {
  return request(HTTP_METHOD_POST, (const uint8_t*)payload.c_str(), payload.length());
}

int HTTPClient::POST(const uint8_t* payload, size_t size) {
  return request(HTTP_METHOD_POST, payload, size);
}

String HTTPClient::getString() {
  if (!handle_) return String();

  String out;
  if (content_length_ > 0) out.reserve((size_t)content_length_ + 1);

  char buffer[512];
  uint32_t deadline = millis() + timeout_ms_ + 2000;
  while (millis() < deadline) {
    int got = esp_http_client_read(handle_, buffer, sizeof(buffer));
    if (got > 0) {
      out.str().append(buffer, (size_t)got);
      if (content_length_ > 0 && (int64_t)out.length() >= content_length_) break;
      continue;
    }
    if (got == 0 && esp_http_client_is_complete_data_received(handle_)) break;
    if (got < 0) break;
    delay(5);
  }
  return out;
}

String HTTPClient::header(const char* name) {
  if (!handle_ || !name) return String();
  char* value = nullptr;
  if (esp_http_client_get_header(handle_, name, &value) != ESP_OK || !value) return String();
  String out(value);
  free(value);
  return out;
}
