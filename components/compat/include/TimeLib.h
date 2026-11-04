// Subset of PaulStoffregen/Time. The ground station feeds it UTC epoch seconds
// from the SNTP-synced system clock, so all accessors decompose UTC - exactly
// what the APRS code expects for its timestamps.
#pragma once

#include <time.h>
#include <stdint.h>

typedef time_t (*getExternalTime)();

typedef enum { timeNotSet, timeNeedsSync, timeSet } timeStatus_t;

void setSyncProvider(getExternalTime provider);
void setSyncInterval(int interval_seconds);
timeStatus_t timeStatus();

time_t now();
void setTime(time_t t);

int hour();
int hour(time_t t);
int minute();
int minute(time_t t);
int second();
int second(time_t t);
int day();
int day(time_t t);
int weekday();
int weekday(time_t t);
int month();
int month(time_t t);
int year();
int year(time_t t);
