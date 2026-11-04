// Placeholder for API compatibility: TLS is handled inside HTTPClient (which
// runs on esp_http_client), so the client object passed to begin() carries no
// state. Kept so the ported OTA code compiles unchanged.
#pragma once

#include "Client.h"

class WiFiClientSecure : public WiFiClient {
 public:
  void setInsecure() { insecure_ = true; }
  bool isInsecure() const { return insecure_; }
  void setCACert(const char*) {}
  void setHandshakeTimeout(unsigned long) {}

 private:
  bool insecure_ = false;
};
