#include "hal.h"
#include <Wire.h>
#include "esp_heap_caps.h"
#include "Freenove_WS2812_Lib_for_ESP32.h"

ST77922 lcd;
ST77922_TOUCH touchpad;

// Onboard WS2812 activity lamp (GPIO40, 1 LED, GRB, RMT ch 0).
static Freenove_ESP32_WS2812 s_led(1, 40, 0, TYPE_GRB);
static volatile uint32_t s_act_deadline = 0;
static volatile uint8_t  s_act_r = 0, s_act_g = 0, s_act_b = 0;
static bool s_led_lit = false;

static const uint32_t UART_TX = 43;
static const uint32_t UART_RX = 44;
static uint32_t current_baud = 115200;

// LVGL draw buffers: 2 x 320x40 px RGB565 in internal DMA-capable RAM
static const uint16_t BUF_LINES = 40;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t* buf1 = nullptr;
static lv_color_t* buf2 = nullptr;

static void disp_flush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  uint16_t w = area->x2 - area->x1 + 1;
  uint16_t h = area->y2 - area->y1 + 1;
  lcd.Fill_Colors(area->x1, area->y1, w, h, (uint16_t*)color_p);
  lv_disp_flush_ready(drv);
}

// ST77922 requirement (per Freenove reference sketch): all invalidated
// areas must be 4-px aligned, else the QSPI pixel stream drifts against
// the panel's column addressing -> diagonal shear on partial redraws.
static void disp_rounder(lv_disp_drv_t* drv, lv_area_t* area) {
  (void)drv;
  area->x1 = area->x1 & ~0x3;
  area->y1 = area->y1 & ~0x3;
  area->x2 = area->x2 | 0x3;
  area->y2 = area->y2 | 0x3;
}

static void touch_read(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  if (touchpad.Get_Touch() && (touchpad.touch.id[0] & 0x1)) {
    uint16_t rx = touchpad.touch.x[0];
    uint16_t ry = touchpad.touch.y[0];
    // Clamp to panel bounds (320x480 portrait, rotation 0 — direct mapping)
    uint16_t mx = rx > (LCD_WIDTH - 1) ? (LCD_WIDTH - 1) : rx;
    uint16_t my = ry > (LCD_HEIGHT - 1) ? (LCD_HEIGHT - 1) : ry;
    // Raw-coordinate debug, throttled: print only on >8px movement so a
    // drag doesn't hammer the USB-CDC console mid-DMA-transfer.
    static uint16_t lx = 0xFFFF, ly = 0xFFFF;
    if (lx == 0xFFFF || abs((int)mx - (int)lx) > 8 || abs((int)my - (int)ly) > 8) {
      Serial.printf("TOUCH raw=%u,%u mapped=%u,%u\n", rx, ry, mx, my);
      lx = mx; ly = my;
    }
    data->state = LV_INDEV_STATE_PR;
    data->point.x = mx;
    data->point.y = my;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

void hal_init() {
  // LCD constructor: QSPI bus + init sequence + backlight on.
  // Touch constructor: Wire.begin(38, 39) @ 100 kHz (shared expansion bus).
  lcd.Set_Rotation(0);
  touchpad.init();
  touchpad.Set_Rotation(0);

  lv_init();

  size_t buf_bytes = LCD_WIDTH * BUF_LINES * sizeof(lv_color_t);
  // Explicit internal SRAM, DMA-capable. NEVER PSRAM: DMA reads from
  // octal PSRAM through the cache corrupt under concurrent peripheral
  // load (touch I2C / USB-CDC). Fall back to a half-size buffer in
  // internal RAM rather than risk a PSRAM placement.
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
  buf1 = (lv_color_t*)heap_caps_malloc(buf_bytes, caps);
  buf2 = (lv_color_t*)heap_caps_malloc(buf_bytes, caps);
  if (!buf1 || !buf2) {
    buf_bytes /= 2;
    buf1 = (lv_color_t*)heap_caps_malloc(buf_bytes, caps);
    buf2 = (lv_color_t*)heap_caps_malloc(buf_bytes, caps);
  }
  Serial.printf("draw bufs: %u bytes each, internal=%d\n",
                (unsigned)buf_bytes,
                buf1 ? (int)heap_caps_get_allocated_size(buf1) != 0 : 0);
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_bytes / sizeof(lv_color_t));

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = LCD_WIDTH;        // hard-locked: 320
  disp_drv.ver_res = LCD_HEIGHT;       // hard-locked: 480
  disp_drv.rotated = LV_DISP_ROT_NONE; // no software rotation; hardware MADCTL 0x00
  disp_drv.sw_rotate = 0;
  disp_drv.full_refresh = 0;
  disp_drv.rounder_cb = disp_rounder;  // ST77922: 4-px aligned windows (Freenove ref)
  disp_drv.flush_cb = disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);

  // UART0 expansion header (43/44) at default baud
  hal_uart_set_baud(current_baud);

  // Activity lamp: dark at boot.
  s_led.begin();
  s_led.setLedColorData(0, 0, 0, 0);
  s_led.show();
}

void hal_task() {
  // Activity lamp service: single-threaded RMT writes from the main loop.
  if (s_act_deadline) {
    if ((int32_t)(millis() - s_act_deadline) < 0) {
      if (!s_led_lit) {
        s_led.setLedColorData(0, s_act_r, s_act_g, s_act_b);
        s_led.show();
        s_led_lit = true;
      }
    } else {
      s_act_deadline = 0;
      if (s_led_lit) {
        s_led.setLedColorData(0, 0, 0, 0);
        s_led.show();
        s_led_lit = false;
      }
    }
  }
  lv_timer_handler();
}

void hal_activity_pulse(uint8_t r, uint8_t g, uint8_t b, uint16_t ms) {
  // Volatile stores only — safe from ISR and worker tasks. If a pulse is
  // already lit, the newest colour/deadline wins (latest instrument speaks).
  s_act_r = r; s_act_g = g; s_act_b = b;
  s_act_deadline = millis() + ms;
  s_led_lit = false;   // force colour refresh on next hal_task
}

void hal_uart_set_baud(uint32_t baud) {
  current_baud = baud;
  Serial0.end();
  Serial0.begin(baud, SERIAL_8N1, UART_RX, UART_TX);
  Serial.printf("UART0 baud set to %lu (8N1 on pins 43/44)\n", (unsigned long)baud);
  Serial.flush();
}

uint32_t hal_uart_get_baud() {
  return current_baud;
}

void hal_i2c_scan(String& out) {
  out = "";
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      char tmp[8];
      snprintf(tmp, sizeof(tmp), "0x%02X ", addr);
      out += tmp;
    }
  }
  if (out.length() == 0) out = "(none)";
}
