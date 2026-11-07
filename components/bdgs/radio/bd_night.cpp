#include "bd_night.h"

#include <math.h>

#include "bd_state.h"
#include "fanet_sched.h"

// Before this the clock has not been set by SNTP yet (2023-11-14).
static constexpr time_t kClockValid = 1700000000;

static constexpr double kPi = 3.14159265358979323846;
static double rad(double deg) { return deg * kPi / 180.0; }
static double deg(double r) { return r * 180.0 / kPi; }

// The sunrise equation with the corrections for refraction and the size of
// the sun's disc (-0.833 deg), good to about a minute away from the poles.
// https://en.wikipedia.org/wiki/Sunrise_equation
void bd_night_sun_times(time_t t, time_t* sunrise, time_t* sunset) {
  const double lat = settings.latitude;
  const double lon = settings.longitude;
  // Julian day of the given UTC day, at noon.
  const double day = floor((double)t / 86400.0);
  const double n = day + 2440587.5 + 0.5 - 2451545.0 + 0.0008;
  const double jStar = n - lon / 360.0;
  const double m = fmod(357.5291 + 0.98560028 * jStar, 360.0);
  const double c = 1.9148 * sin(rad(m)) + 0.02 * sin(rad(2 * m)) + 0.0003 * sin(rad(3 * m));
  const double lambda = fmod(m + c + 180.0 + 102.9372, 360.0);
  const double transit = 2451545.0 + jStar + 0.0053 * sin(rad(m)) - 0.0069 * sin(rad(2 * lambda));
  const double sinDecl = sin(rad(lambda)) * sin(rad(23.4397));
  const double cosDecl = cos(asin(sinDecl));
  const double cosHour = (sin(rad(-0.833)) - sin(rad(lat)) * sinDecl) / (cos(rad(lat)) * cosDecl);
  if (cosHour < -1.0) {  // midnight sun
    *sunrise = *sunset = 0;
    return;
  }
  if (cosHour > 1.0) {  // polar night
    *sunrise = *sunset = -1;
    return;
  }
  const double hourAngle = deg(acos(cosHour));
  *sunrise = (time_t)llround((transit - hourAngle / 360.0 - 2440587.5) * 86400.0);
  *sunset = (time_t)llround((transit + hourAngle / 360.0 - 2440587.5) * 86400.0);
}

// The night around t: from the sunset of one day to the sunrise of the next.
// Days are UTC days, so far west or east the night that matters may belong to
// the day before or after; all three are looked at.
static bool night_around(time_t t, time_t* from, time_t* to) {
  for (int d = -1; d <= 1; d++) {
    time_t rise, set, nextRise, nextSet;
    bd_night_sun_times(t + d * 86400, &rise, &set);
    bd_night_sun_times(t + (d + 1) * 86400, &nextRise, &nextSet);
    if (set <= 0 || nextRise <= 0) continue;  // no ordinary night here
    if (t >= set && t < nextRise) {
      *from = set;
      *to = nextRise;
      return true;
    }
  }
  return false;
}

bool bd_night_active() {
  if (!settings.nightSaving) return false;
  const time_t now = time(nullptr);
  if (now < kClockValid) return false;
  // The calculation is cheap but runs on every pass of the radio task.
  static time_t s_checked = 0;
  static bool s_night = false;
  if (now != s_checked) {
    s_checked = now;
    time_t from, to;
    s_night = night_around(now, &from, &to);
  }
  return s_night;
}

void bd_night_window(time_t* sunset, time_t* sunrise) {
  *sunset = *sunrise = 0;
  const time_t now = time(nullptr);
  if (now < kClockValid) return;
  if (night_around(now, sunset, sunrise)) return;
  // Daytime: tonight's.
  for (int d = 0; d <= 1; d++) {
    time_t rise, set, nextRise, nextSet;
    bd_night_sun_times(now + d * 86400, &rise, &set);
    bd_night_sun_times(now + (d + 1) * 86400, &nextRise, &nextSet);
    if (set > now && nextRise > 0) {
      *sunset = set;
      *sunrise = nextRise;
      return;
    }
  }
}

int bd_night_synced_stations() {
  SchedInfo list[SCHED_MAX_STATIONS];
  const uint8_t n = sched_snapshot(list, SCHED_MAX_STATIONS);
  int synced = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (list[i].state == SCHED_SYNCED) synced++;
  }
  return synced;
}
