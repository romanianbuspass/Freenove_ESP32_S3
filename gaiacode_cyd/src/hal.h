#pragma once
#include <Arduino.h>
#include <lvgl.h>
#include "ST77922.h"
#include "ST77922_Touch.h"

// GaiaCode CYD HAL — Freenove FNK0104N
// Display: ST77922 QSPI (CS=10, SCLK=12, D0-D3=11/13/14/9, BL=41)
// Touch:   ST77922 TDDI @ I2C 0x55 (SCL=39, SDA=38, RST=48, INT=47, 100 kHz)
// UART:    UART0 header TXD0=43 / RXD0=44, variable baud
// I2C ext: shares touch bus on 39/38 (global Wire)

extern ST77922 lcd;
extern ST77922_TOUCH touchpad;

void hal_init();                 // display + touch + LVGL + backlight
void hal_task();                 // lv_timer_handler pump (call from loop)

// Onboard WS2812 (GPIO40) activity lamp. Thread-safe: only sets volatiles;
// the actual RMT write happens in hal_task(). Pulse `ms` milliseconds.
void hal_activity_pulse(uint8_t r, uint8_t g, uint8_t b, uint16_t ms);

void     hal_uart_set_baud(uint32_t baud);
uint32_t hal_uart_get_baud();

// Scan the shared expansion I2C bus; appends "0xNN " per found address.
void hal_i2c_scan(String& out);
