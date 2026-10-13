#pragma once
#include <stdint.h>

// GaiaCode Universal Sensor Terminal — profile model.
// Faithful port of SENSOR_PROFILES from Seismo Tools/Sensor_Terminal.py.
// Display names are ASCII-safe (Montserrat lacks θ/Σ/α glyphs).

struct SensorButton {
  const char* label;
  const char* cmd;    // transmitted verbatim + "\r\n"
};

enum SensorGroupType : uint8_t {
  GROUP_AXIS = 0,
  GROUP_MASS,
  GROUP_GAIN,
  GROUP_ADV,
};

struct SensorGroup {
  const char* title;
  SensorGroupType type;
  const SensorButton* buttons;
  uint8_t button_count;
};

struct SensorProfile {
  const char* key;    // "pico" | "theta" | "sigma" | "alpha"
  const char* name;   // display name
  const SensorGroup* groups;
  uint8_t group_count;
};

extern const SensorProfile SENSOR_PROFILES[];
extern const uint8_t SENSOR_PROFILE_COUNT;

// Auto-detect (port of SensorTerminalApp._try_auto_detect_sensor):
// line must start with "GAIA"; ALPHA->alpha, SIGMA->sigma,
// Theta->theta unless the line also contains "Pico" (->pico).
// Returns profile index, or -1 when the line matches nothing.
int sensor_autodetect(const char* line);
