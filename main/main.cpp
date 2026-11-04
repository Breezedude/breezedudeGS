// Breezedude FANET ground station - ESP-IDF entry point.
//
// app_main only brings up the pieces every later stage depends on and then
// hands control to the task set in bd_app; there is no Arduino-style loop().
#include "Arduino.h"
#include "bd_app.h"
#include "esp_log.h"

extern "C" void app_main(void) {
  Serial.begin(115200);
  Serial.setDebugOutput(true);

  bd_app_start();
}
