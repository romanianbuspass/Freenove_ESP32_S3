#include "sensor_profiles.h"
#include <string.h>

// ---- Button tables (verbatim from Sensor_Terminal.py) --------------------

static const SensorButton BTN_AXIS[] = {
  {"All Axes", "A"}, {"North/South", "N"}, {"East/West", "E"}, {"Vertical", "Z"},
};

// Pico
static const SensorButton PICO_MASS[] = {
  {"Centre Mass", "C"}, {"Show Mass", "M"}, {"Motor (-)", "<"}, {"Motor (+)", ">"},
  {"Cal Enable ON", "I"}, {"Cal Enable OFF", "K"}, {"All Cal ON", "T"}, {"All Cal OFF", "R"},
};
static const SensorButton PICO_GAIN[] = {
  {"Vel * 1", "1V"}, {"Vel * 2", "2V"}, {"Vel * 4", "4V"}, {"Vel * 10", "10V"},
  {"Acc * 1", "1A"}, {"Acc * 5", "5A"}, {"Acc * 10", "10A"}, {"Acc * 50", "50A"},
};
static const SensorButton PICO_ADV[] = {
  {"Short (30s)", "S"}, {"Mid (60s)", "L"}, {"Long (120s)", "B"},
  {"Show Params", "D"}, {"Store Params", "Q"}, {"Save ADC Zero", "9"}, {"Read ADC Zero", "%"},
};

// Theta
static const SensorButton THETA_MASS[] = {
  {"Centre Mass", "C"}, {"Show Mass", "M"}, {"Motor (-)", "<"}, {"Motor (+)", ">"},
  {"Zero Mass Pos", "="}, {"Cal Enable ON", "I"}, {"Cal Enable OFF", "K"},
};
static const SensorButton THETA_GAIN[] = {
  {"Low Gain", "G"}, {"High Gain", "H"},
};
static const SensorButton THETA_ADV[] = {
  {"Sign-On Msg", "?"}, {"Short Period", "S"}, {"Long Period", "B"}, {"Show Params", "D"},
};

// Sigma
static const SensorButton SIGMA_MASS[] = {
  {"Centre Mass", "C"}, {"Show Mass", "M"}, {"Motor (-)", "<"}, {"Motor (+)", ">"},
  {"Zero Mass Pos", "="},
};
static const SensorButton SIGMA_GAIN[] = {
  {"0.5g Gain", "1G"}, {"1g Gain", "2G"}, {"2g Gain", "3G"}, {"4g Gain", "4G"},
};
static const SensorButton SIGMA_ADV[] = {
  {"Sign-On Msg", "?"}, {"Show Params", "D"},
};

// Alpha
static const SensorButton ALPHA_MASS[] = {
  {"Centre Mass", "C"}, {"Show Mass", "M"}, {"Motor (-)", "<"}, {"Motor (+)", ">"},
  {"Zero Mass Pos", "="}, {"Cal Enable ON", "I"}, {"Cal Enable OFF", "K"},
};
static const SensorButton ALPHA_ADV[] = {
  {"Sign-On Msg", "?"}, {"Short Period", "S"}, {"Long Period", "B"}, {"Show Params", "D"},
  {"Store Params", "Q"}, {"Lock Axes", "L"}, {"Unlock Axes", "U"},
};

// ---- Group tables ---------------------------------------------------------

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const SensorGroup PICO_GROUPS[] = {
  {"1. Target Axis Select", GROUP_AXIS, BTN_AXIS, COUNT(BTN_AXIS)},
  {"2. Mass & Calibration", GROUP_MASS, PICO_MASS, COUNT(PICO_MASS)},
  {"3. Gain Control", GROUP_GAIN, PICO_GAIN, COUNT(PICO_GAIN)},
  {"4. Advanced System", GROUP_ADV, PICO_ADV, COUNT(PICO_ADV)},
};
static const SensorGroup THETA_GROUPS[] = {
  {"1. Target Axis Select", GROUP_AXIS, BTN_AXIS, COUNT(BTN_AXIS)},
  {"2. Mass & Calibration", GROUP_MASS, THETA_MASS, COUNT(THETA_MASS)},
  {"3. Gain Control", GROUP_GAIN, THETA_GAIN, COUNT(THETA_GAIN)},
  {"4. Advanced System", GROUP_ADV, THETA_ADV, COUNT(THETA_ADV)},
};
static const SensorGroup SIGMA_GROUPS[] = {
  {"1. Target Axis Select", GROUP_AXIS, BTN_AXIS, COUNT(BTN_AXIS)},
  {"2. Mass & Calibration", GROUP_MASS, SIGMA_MASS, COUNT(SIGMA_MASS)},
  {"3. Gain Control", GROUP_GAIN, SIGMA_GAIN, COUNT(SIGMA_GAIN)},
  {"4. Advanced System", GROUP_ADV, SIGMA_ADV, COUNT(SIGMA_ADV)},
};
static const SensorGroup ALPHA_GROUPS[] = {
  {"1. Target Axis Select", GROUP_AXIS, BTN_AXIS, COUNT(BTN_AXIS)},
  {"2. Mass & Calibration", GROUP_MASS, ALPHA_MASS, COUNT(ALPHA_MASS)},
  {"3. Advanced System", GROUP_ADV, ALPHA_ADV, COUNT(ALPHA_ADV)},
};

// ---- Profile table ---------------------------------------------------------

const SensorProfile SENSOR_PROFILES[] = {
  {"pico",  "Theta Pico", PICO_GROUPS,  COUNT(PICO_GROUPS)},
  {"theta", "Theta",      THETA_GROUPS, COUNT(THETA_GROUPS)},
  {"sigma", "Sigma",      SIGMA_GROUPS, COUNT(SIGMA_GROUPS)},
  {"alpha", "Alpha",      ALPHA_GROUPS, COUNT(ALPHA_GROUPS)},
};
const uint8_t SENSOR_PROFILE_COUNT = COUNT(SENSOR_PROFILES);

// ---- Auto-detect ------------------------------------------------------------

int sensor_autodetect(const char* line) {
  if (!line || strncmp(line, "GAIA", 4) != 0) return -1;
  if (strstr(line, "ALPHA")) return 3;                    // alpha
  if (strstr(line, "SIGMA")) return 2;                    // sigma
  if (strstr(line, "Theta"))
    return strstr(line, "Pico") ? 0 : 1;                  // pico / theta
  return -1;
}
