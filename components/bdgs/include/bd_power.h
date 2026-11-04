// Power management: dynamic frequency scaling, automatic light sleep, battery
// monitoring and the scheduled deep-sleep window.
//
// Light sleep is what this port is for. It works because every task blocks:
// the radio task on the DIO1 interrupt, the net task on a notification, the web
// server on its sockets. The idle task then powers the CPU down and the chip
// wakes on a LoRa packet, a WiFi beacon or the next scheduler deadline.
#pragma once

#include <Arduino.h>

void bd_power_init();
void bd_power_start();

float getBatteryVoltage();
void configureTimeZoneFromLocation();

// On a deep-sleep wake with a depleted battery, go straight back to sleep
// instead of bringing up radio and WiFi.
void handleLowBatteryAtStartup();

// Holds off light sleep while something latency critical is running (firmware
// upload, LoRa OTA session). Reference counted.
void bd_power_hold_awake(bool hold);

// Human readable reason the station is (not) sleeping, for the web UI.
String bd_power_state_text();
