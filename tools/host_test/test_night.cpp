// Host test for the night mode: the sunrise/sunset calculation in
// radio/bd_night.cpp and the ground station's changes to radio/fanet_sched.cpp
// (the scheduler itself is tested in breezedude_WindSensor
// testing/host/fanet_sched, against the sensor's copy).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "bd_night.h"
#include "bd_state.h"
#include "fanet_sched.h"

Settings settings;
void log_i(const char *) {}

static int g_fail = 0;
static void check(bool cond, const char *what) {
  printf("  %-66s %s\n", what, cond ? "ok" : "FAIL");
  if (!cond) g_fail++;
}

// UTC epoch of a date and time.
static time_t utc(int y, int mo, int d, int h, int mi) {
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
#ifdef _WIN32
  return _mkgmtime(&t);
#else
  return timegm(&t);
#endif
}

static bool near(time_t got, time_t want, int minutes) { return std::llabs((long long)(got - want)) <= minutes * 60; }

int main() {
  time_t rise, set;

  printf("sun times\n");
  // Innsbruck, reference times from the NOAA solar calculator equations.
  settings.latitude = 47.269f;
  settings.longitude = 11.404f;
  bd_night_sun_times(utc(2026, 11, 7, 12, 0), &rise, &set);
  check(near(rise, utc(2026, 11, 7, 6, 6), 3), "Innsbruck 7 Nov: sunrise 06:06 UTC");
  check(near(set, utc(2026, 11, 7, 15, 49), 3), "Innsbruck 7 Nov: sunset 15:49 UTC");
  bd_night_sun_times(utc(2026, 6, 21, 12, 0), &rise, &set);
  check(near(rise, utc(2026, 6, 21, 3, 18), 3), "Innsbruck 21 Jun: sunrise 03:18 UTC");
  check(near(set, utc(2026, 6, 21, 19, 14), 3), "Innsbruck 21 Jun: sunset 19:14 UTC");

  // Tromso: midnight sun in June, polar night in December.
  settings.latitude = 69.65f;
  settings.longitude = 18.96f;
  bd_night_sun_times(utc(2026, 6, 21, 12, 0), &rise, &set);
  check(rise == 0 && set == 0, "Tromso 21 Jun: midnight sun");
  bd_night_sun_times(utc(2026, 12, 21, 12, 0), &rise, &set);
  check(rise == -1 && set == -1, "Tromso 21 Dec: polar night");

  printf("night window\n");
  settings.latitude = 47.269f;
  settings.longitude = 11.404f;
  // bd_night_active() and bd_night_window() read the clock; the window logic is
  // checked through bd_night_sun_times() of consecutive days instead.
  time_t rise2, set2;
  bd_night_sun_times(utc(2026, 11, 7, 12, 0), &rise, &set);
  bd_night_sun_times(utc(2026, 11, 8, 12, 0), &rise2, &set2);
  check(set < rise2 && rise2 - set > 13 * 3600, "night of 7/8 Nov lasts more than 13 h");

  printf("scheduler, ground station changes\n");
  sched_configure(3600000u, 0);  // no scan window, it would keep the receiver on
  const uint32_t id = 0x11BD01;
  const uint32_t period = 40000;
  uint32_t t = 1000000;
  sched_note_weather(id, t);
  sched_note_weather(id, t + period);
  sched_note_weather(id, t + 2 * period);
  SchedInfo s[SCHED_MAX_STATIONS];
  uint8_t n = sched_snapshot(s, SCHED_MAX_STATIONS);
  check(n == 1 && s[0].state == SCHED_SYNCED, "station synced after three packets");
  const uint32_t next = s[0].next;
  // A repeater's copy of the same packet arrives 1.5 s later.
  sched_note_weather(id, t + 2 * period + 1500);
  sched_snapshot(s, SCHED_MAX_STATIONS);
  check(s[0].next == next && s[0].state == SCHED_SYNCED, "repeated copy does not move the prediction");
  check(s[0].hits == 3, "repeated copy is not counted");
  check(!sched_rx_wanted(t + 2 * period + 20000), "receiver off between two packets");
  check(sched_rx_wanted(next - 100), "receiver on just before the next packet");

  // Fill the table: a ground station hears many stations.
  for (uint32_t i = 1; i < 32; i++) sched_note_weather(0x110000 + i, t + 2 * period + 2000 + i);
  check(sched_snapshot(s, SCHED_MAX_STATIONS) == 32, "32 stations fit");
  check(!sched_dropped_take(), "none dropped to make room");

  printf("%s\n", g_fail ? "FAILED" : "all passed");
  return g_fail ? 1 : 0;
}
