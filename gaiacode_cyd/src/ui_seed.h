#pragma once
#include <lvgl.h>

// GaiaCode CYD — miniSEED waveform viewer tab (Phase 6).
// SeedLink client UI: Tau picker (from the LAN scanner), editable
// NET.STA.LOC.CHAN selector (default XX.????.??.F??), live decimated traces.
void ui_seed_build(lv_obj_t* tab);
