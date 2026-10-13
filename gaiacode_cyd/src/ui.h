#pragma once
#include <lvgl.h>

// GaiaCode CYD UI — MD3 splash, then the Universal Sensor Terminal:
// profile selector + baud selector header, dynamic themed button grid,
// UART0 command TX and auto-detect RX polling.
void ui_init();   // builds splash; terminal replaces it after a short dwell
void ui_show_main();   // return from a sub-screen to the main tabview
