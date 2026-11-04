#include "TimeLib.h"

#include <sys/time.h>

static getExternalTime s_provider = nullptr;

// The system clock itself is the time source here (SNTP keeps it honest), so
// the sync interval only decides how often we consult the provider.
void setSyncProvider(getExternalTime provider) { s_provider = provider; }

void setSyncInterval(int) {}

time_t now() {
  if (s_provider) return s_provider();
  return time(nullptr);
}

void setTime(time_t t) {
  struct timeval tv = {};
  tv.tv_sec = t;
  settimeofday(&tv, nullptr);
}

timeStatus_t timeStatus() {
  // Anything before 2001 means SNTP has not delivered yet.
  return now() > 978307200 ? timeSet : timeNotSet;
}

static struct tm breakTime(time_t t) {
  struct tm parts = {};
  gmtime_r(&t, &parts);
  return parts;
}

int hour() { return hour(now()); }
int hour(time_t t) { return breakTime(t).tm_hour; }
int minute() { return minute(now()); }
int minute(time_t t) { return breakTime(t).tm_min; }
int second() { return second(now()); }
int second(time_t t) { return breakTime(t).tm_sec; }
int day() { return day(now()); }
int day(time_t t) { return breakTime(t).tm_mday; }
int weekday() { return weekday(now()); }
int weekday(time_t t) { return breakTime(t).tm_wday + 1; }  // Time library counts Sunday as 1
int month() { return month(now()); }
int month(time_t t) { return breakTime(t).tm_mon + 1; }
int year() { return year(now()); }
int year(time_t t) { return breakTime(t).tm_year + 1900; }
