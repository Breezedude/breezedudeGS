// Radio task: owns the single transceiver.
//
// The DIO1 interrupt is both the packet notification and the light-sleep wake
// source, so between packets (and between FLARM windows) this task is blocked
// and the CPU is free to sleep.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "helper.h"

// Starts the transceiver and the receive task. Returns false when no radio
// module answered - the firmware then runs on as a WiFi-only station, same as
// the Arduino build did.
bool bd_radio_start();

// Exclusive access for code that drives the radio itself (the LoRa OTA gateway
// switches modem parameters and runs its own receive loop).
void bd_radio_lock();
void bd_radio_unlock();

// Wakes the radio task so it re-evaluates the schedule, e.g. after the web UI
// changed which protocols should be received.
void bd_radio_notify();

// Smoothed channel RSSI between packets in dBm, NAN until FANET reception has
// sampled it.
float bd_radio_noise_floor();

// Receive paths, called from the radio task.
void handle_fanet();
#if CONFIG_BDGS_ENABLE_FSK_RX
void handle_flarm();
void handle_adsl();
#endif
