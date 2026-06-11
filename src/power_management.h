#pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include "types.h"
#include "aprs.h"

void initPowerManagementHardware();
void disableBluetoothForPowerSave();
void configureTimeZoneFromLocation();
float getBatteryVoltage();
void handleLowBatteryAtStartup();
void handleBatteryAndSleep();
