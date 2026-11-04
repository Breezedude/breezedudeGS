// Shared runtime state.
//
// The symbol names are the ones the ported modules (aprs.cpp, ota_gs.cpp,
// config_gs.cpp) already reference, so they keep compiling untouched.
#pragma once

#include <Arduino.h>

#include "aprs.h"
#include "types.h"

// --- configuration ----------------------------------------------------------
extern Settings settings;
void load_preferences();
void save_preferences();

// Guards the settings struct against concurrent access from the web, radio and
// housekeeping tasks. Writers hold it; readers of single scalars do not bother.
void settings_lock();
void settings_unlock();

// --- firmware / update state ------------------------------------------------
extern String fw_version;
extern bool update_available;
extern String update_available_version;
extern bool force_update_check;
void onUpdateBranchChanged();

// "stable" and "beta" always; "alpha" only in builds with
// CONFIG_BDGS_FW_ALPHA_CHANNEL (internal pre-release testing).
bool update_branch_allowed(const char* branch);

// --- connectivity -----------------------------------------------------------
extern bool wifiConnected;
extern bool forceReconnectSTA;
extern bool restartAP;
extern volatile bool otaUploadInProgress;
extern volatile bool otaUploadRejected;
extern const char* ntpServer;
extern String breezedudeUrl;
extern String breezedudeHwInfoUrl;

// --- APRS -------------------------------------------------------------------
extern Aprs aprs;
void update_aprs_settings();

// The APRS client was written for a single-threaded loop(): its socket is now
// touched by the APRS task (run) and by the radio task (sendTrackingData,
// sendWeatherData), so every call goes through this lock.
void aprs_lock();
void aprs_unlock();

// --- station data -----------------------------------------------------------
extern uint32_t fanet_rx_count;
String getDeviceName(uint8_t vid, uint16_t fanet_id);
void setDeviceName(uint8_t vid, uint16_t fanet_id, String name);

// The three stores are written by the radio task and read by the web task.
void store_lock();
void store_unlock();

// --- web UI verbosity switches ---------------------------------------------
extern bool ota_verbose_ws;
extern bool flarm_verbose_ws;
extern bool adsl_verbose_ws;
