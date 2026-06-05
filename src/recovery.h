#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

void migratePartitionLayoutIfNeeded();

void registerRecoveryRoutes(
  AsyncWebServer& server,
  AsyncWebSocket& ws,
  bool& littlefsMounted,
  volatile bool& otaUploadInProgress,
  volatile bool& otaUploadRejected
);
