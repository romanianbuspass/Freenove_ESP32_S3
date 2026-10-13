#include <Arduino.h>
#include "esp_system.h"
#include "hal.h"
#include "ui.h"
#include "storage.h"
#include "net.h"
#include "gnss.h"
#include "audio.h"
#include "tau_config.h"
#include "slink.h"

// GaiaCode CYD — Freenove FNK0104N (ESP32-S3R8, 3.5" ST77922 QSPI)
// Splash (2.5 s) -> MD3 tabbed UI: Sensor / GNSS / LAN / ICSP.

// Boot-up reset diagnostics: identify watchdog/panic reboots before any
// peripheral init touches the console. The formatted line is also stashed
// so setup() can persist it to the SD log once storage_init() has run —
// panic/watchdog text goes to the USB-S/J ROM console, which is not the
// enumerated CDC port while the app runs, so serial capture alone loses it.
static char s_rr_log[96] = "";

static void log_reset_reason() {
  esp_reset_reason_t rr = esp_reset_reason();
  const char* name;
  switch (rr) {
    case ESP_RST_POWERON:   name = "power-on"; break;
    case ESP_RST_EXT:       name = "external pin"; break;
    case ESP_RST_SW:        name = "software (esp_restart)"; break;
    case ESP_RST_PANIC:     name = "CORE PANIC / exception"; break;
    case ESP_RST_INT_WDT:   name = "INTERRUPT WATCHDOG"; break;
    case ESP_RST_TASK_WDT:  name = "TASK WATCHDOG"; break;
    case ESP_RST_WDT:       name = "other watchdog"; break;
    case ESP_RST_DEEPSLEEP: name = "deep-sleep wake"; break;
    case ESP_RST_BROWNOUT:  name = "brownout"; break;
    case ESP_RST_SDIO:      name = "SDIO"; break;
    default:                name = "unknown"; break;
  }
  Serial.printf("BOOT: reset reason %d (%s)\n", (int)rr, name);
  snprintf(s_rr_log, sizeof(s_rr_log), "reset reason %d (%s)", (int)rr, name);
  if (rr == ESP_RST_INT_WDT)
    Serial.println("BOOT: !! Interrupt Watchdog reset — suspect ISR storm (1 PPS?)");
  if (rr == ESP_RST_TASK_WDT)
    Serial.println("BOOT: !! Task Watchdog reset — suspect a starved/blocked task");
  if (rr == ESP_RST_PANIC)
    Serial.println("BOOT: !! Core Panic — see backtrace in previous boot output");
}

void setup() {
  Serial.begin(115200);  // USB-CDC console
  delay(300);
  Serial.println("GaiaCode CYD boot");
  log_reset_reason();

  hal_init();
  audio_init();    // I2S + ES8311 (needs Wire from hal_init's touch init)
  storage_init();  // SD backbone (Phase 2) — serial report on USB-CDC
  if (s_rr_log[0]) storage_log("BOOT", s_rr_log);  // persist reset reason
  net_init();      // Wi-Fi STA + discovery probe task (Phase 3)
  tau_config_init(); // Tau HTTP configurator worker (Phase 5)
  slink_init();    // SeedLink waveform client worker (Phase 6)
  gnss_init();     // UART2 NMEA parser (Phase 4)
  ui_init();

  // Reprint the SD report ~5 s in: native USB re-enumerates on reset, so a
  // host monitor typically attaches AFTER the boot-time report has gone out.
  lv_timer_create([](lv_timer_t* t){ storage_report(); lv_timer_del(t); },
                  5000, NULL);

  Serial.println("HAL + UI up");
  storage_log("BOOT", "GaiaCode CYD boot complete");
}

void loop() {
  hal_task();
  delay(5);
}
