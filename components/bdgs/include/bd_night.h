// Night mode: between sunset and sunrise there is no paraglider tracking to
// receive, only weather stations, and those send on a fixed clock. With the
// setting on, the radio sleeps between the moments a station's packet is due
// (fanet_sched.h) and the CPU light-sleeps with it. FLARM and ADS-L are off
// for the night.
//
// The rhythms are learned all day from what is received anyway, so the plan is
// ready at sunset. At night a scan window - six minutes every hour - finds
// stations that appeared since and catches their name packets.
#pragma once

#include <stdint.h>
#include <time.h>

// Sunrise and sunset (UTC epoch) of the UTC day that contains t, for the
// station's position. Both 0 for midnight sun, both -1 for polar night.
void bd_night_sun_times(time_t t, time_t* sunrise, time_t* sunset);

// True while the setting is on, the clock is set and the sun is down.
bool bd_night_active();

// The night that is running or comes next: its sunset and the sunrise that
// ends it. 0 when there is none (midnight sun) or the clock is not set.
void bd_night_window(time_t* sunset, time_t* sunrise);

// Weather stations whose rhythm is known well enough to sleep between them.
int bd_night_synced_stations();
