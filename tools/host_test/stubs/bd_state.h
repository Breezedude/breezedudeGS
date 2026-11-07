// Host stub: bd_night.cpp only reads the position and the switch.
#pragma once

struct Settings {
  float latitude = 47.0f;
  float longitude = 12.0f;
  bool nightSaving = true;
};
extern Settings settings;
