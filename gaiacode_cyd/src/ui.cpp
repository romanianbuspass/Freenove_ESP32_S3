#include "ui.h"
#include "ui_tau_config.h"
#include "ui_seed.h"
#include "hal.h"
#include "net.h"
#include "gnss.h"
#include "storage.h"
#include "audio.h"
#include "gaiacode_logo.h"
#include "sensor_profiles.h"
#include <string.h>

// ---- GaiaCode MD3 tokens ------------------------------------------------
#define GC_PRIMARY        lv_color_hex(0x0EADB2)  // GaiaCode Teal
#define GC_ON_PRIMARY     lv_color_hex(0x0F172A)
#define GC_BACKGROUND     lv_color_hex(0x0F172A)  // dark navy
#define GC_SURFACE        lv_color_hex(0x1E293B)
#define GC_SURFACE_CONT   lv_color_hex(0x263245)
#define GC_ON_SURFACE     lv_color_hex(0xE2E8F0)
#define GC_ON_SURFACE_VAR lv_color_hex(0x94A3B8)

// ---- Group themes (verbatim from Sensor_Terminal.py THEMES) -------------
static const uint32_t GROUP_BG[4] = {
  0x1E5A7A,  // axis: Dark Teal Blue
  0x8B5E3C,  // mass: Dark Warm Brown
  0x5E3D7A,  // gain: Dark Purple
  0x4A5568,  // adv:  Dark Slate
};
static const uint32_t GROUP_BG_ACTIVE[4] = {
  0x267A9E, 0xA87348, 0x7A4FA0, 0x606D80,
};

// ---- Geometry -----------------------------------------------------------
// Panel 320x480. Top tab bar 40 px; tab content 440 px.
static const lv_coord_t TAB_BAR_H   = 40;
static const lv_coord_t TAB_H       = 480 - 40;   // tab content height
static const lv_coord_t HDR_H       = 96;          // sensor header block
static const lv_coord_t TERM_H      = 80;          // bottom RX readout

// ---- State ----------------------------------------------------------------
static lv_obj_t* scr_splash;
static lv_obj_t* scr_main;      // hosts the tabview
static lv_obj_t* tabview;
static lv_obj_t* dd_profile;
static lv_obj_t* lbl_status;
static lv_obj_t* col_groups;
static lv_obj_t* txt_term;
static int cur_profile = 0;  // pico

static const char* BAUD_OPTS = "9600\n19200\n38400\n57600\n115200";
static const uint32_t BAUD_VALS[] = {9600, 19200, 38400, 57600, 115200};

// ---- Command transmission (port of send_cmd) ------------------------------
static void send_cmd(const SensorButton* b) {
  // Wake the sensor first — blank line exits sleep mode (150 ms pause).
  Serial0.write("\r\n");
  delay(150);
  Serial0.print(b->cmd);
  Serial0.print("\r\n");
  lv_label_set_text_fmt(lbl_status, "TX: %s  (%s)", b->cmd, b->label);
}

static void cmd_btn_cb(lv_event_t* e) {
  send_cmd((const SensorButton*)lv_event_get_user_data(e));
}

// Haptic-style audio feedback on the sensor matrix (80% amplitude).
static void click_beep_cb(lv_event_t* e) {
  (void)e;
  audio_click(80);
}

// FM synth bindings: zap on navigation, kick on primary actions.
static void nav_zap_cb(lv_event_t* e) {
  (void)e;
  audio_play(SOUND_NAV_ZAP);
}

// ---- Dynamic button grid ---------------------------------------------------
static void build_groups() {
  lv_obj_clean(col_groups);
  const SensorProfile* p = &SENSOR_PROFILES[cur_profile];

  for (uint8_t g = 0; g < p->group_count; g++) {
    const SensorGroup* grp = &p->groups[g];

    lv_obj_t* title = lv_label_create(col_groups);
    lv_label_set_text(title, grp->title);
    lv_obj_set_style_text_color(title, lv_color_hex(GROUP_BG_ACTIVE[grp->type]), 0);

    lv_obj_t* row = lv_obj_create(col_groups);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_row(row, 8, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);

    for (uint8_t i = 0; i < grp->button_count; i++) {
      const SensorButton* b = &grp->buttons[i];
      lv_obj_t* btn = lv_btn_create(row);
      lv_obj_set_size(btn, LV_SIZE_CONTENT, 40);
      lv_obj_set_style_pad_hor(btn, 14, 0);
      lv_obj_set_style_radius(btn, 8, 0);
      lv_obj_set_style_bg_color(btn, lv_color_hex(GROUP_BG[grp->type]), 0);
      lv_obj_set_style_bg_color(btn, lv_color_hex(GROUP_BG_ACTIVE[grp->type]),
                                LV_STATE_PRESSED);
      lv_obj_add_event_cb(btn, cmd_btn_cb, LV_EVENT_CLICKED, (void*)b);
      lv_obj_add_event_cb(btn, click_beep_cb, LV_EVENT_PRESSED, NULL);
      lv_obj_t* l = lv_label_create(btn);
      lv_label_set_text(l, b->label);
      lv_obj_set_style_text_color(l, lv_color_white(), 0);
      lv_obj_center(l);
    }
  }
}

// ---- Profile / baud selectors ---------------------------------------------
static void profile_cb(lv_event_t* e) {
  cur_profile = lv_dropdown_get_selected(lv_event_get_target(e));
  build_groups();
  audio_play(SOUND_NAV_ZAP);   // navigation zap
}

static void baud_cb(lv_event_t* e) {
  uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
  hal_uart_set_baud(BAUD_VALS[sel]);
  audio_play(SOUND_NAV_ZAP);   // navigation zap
}

// ---- Terminal readout (RX) -------------------------------------------------
static void term_append(const char* text) {
  if (!txt_term || !text) return;
  lv_textarea_add_text(txt_term, text);
  lv_textarea_add_char(txt_term, '\n');
  // Keep a bounded buffer: trim from front if it grows too long
  const size_t max_len = 800;
  const char* all = lv_textarea_get_text(txt_term);
  size_t len = strlen(all);
  if (len > max_len) {
    size_t drop = len - max_len;
    // advance to next newline so we cut whole lines
    const char* p = all + drop;
    while (*p && *p != '\n') ++p;
    if (*p == '\n') ++p;
    lv_textarea_set_text(txt_term, p);
  }
  lv_textarea_set_cursor_pos(txt_term, LV_TEXTAREA_CURSOR_LAST);
}

// ---- RX polling + auto-detect ----------------------------------------------
// Ping-fault tokens in sensor replies. ASSUMPTION: the exact fault token for
// the impulse-response ("ping") test is not yet defined in our protocols —
// extend this list when the real token is known.
static const char* FAULT_TOKENS[] = { "FAULT", "FAIL", "PING:0", "PING ERR" };

static void handle_rx_line(const char* line) {
  term_append(line);
  for (uint8_t i = 0; i < sizeof(FAULT_TOKENS) / sizeof(FAULT_TOKENS[0]); i++) {
    if (strstr(line, FAULT_TOKENS[i])) {
      audio_play(SOUND_PING_FAULT);   // NeuroFunk growl on ping fault
      break;
    }
  }
  int idx = sensor_autodetect(line);
  if (idx >= 0 && idx != cur_profile) {
    cur_profile = idx;
    lv_dropdown_set_selected(dd_profile, idx);
    build_groups();
    lv_label_set_text_fmt(lbl_status, "Auto-detected: %s", SENSOR_PROFILES[idx].name);
  }
}

static void rx_timer(lv_timer_t* t) {
  (void)t;
  static char line[160];
  static uint8_t len = 0;
  if (Serial0.available()) hal_activity_pulse(0, 32, 28, 60);  // teal: sensor
  while (Serial0.available()) {
    char c = (char)Serial0.read();
    if (c == '\r' || c == '\n') {
      if (len > 0) {
        line[len] = '\0';
        handle_rx_line(line);
        len = 0;
      }
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    }
  }
}

// ---- Tab 1: Sensor Control (moved verbatim, reparented into the tab) --------
static lv_obj_t* make_dropdown(lv_obj_t* parent, const char* opts, lv_coord_t w,
                               lv_event_cb_t cb) {
  lv_obj_t* dd = lv_dropdown_create(parent);
  lv_dropdown_set_options(dd, opts);
  lv_obj_set_width(dd, w);
  lv_obj_set_style_bg_color(dd, GC_SURFACE_CONT, 0);
  lv_obj_set_style_text_color(dd, GC_ON_SURFACE, 0);
  lv_obj_set_style_bg_color(lv_dropdown_get_list(dd), GC_SURFACE, 0);
  lv_obj_set_style_text_color(lv_dropdown_get_list(dd), GC_ON_SURFACE, 0);
  if (cb) lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
  return dd;
}

static void build_sensor_tab(lv_obj_t* tab) {
  // Header: selectors row + status line
  lv_obj_t* hdr = lv_obj_create(tab);
  lv_obj_set_size(hdr, LV_PCT(100), HDR_H);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(hdr, GC_SURFACE, 0);
  lv_obj_set_style_radius(hdr, 0, 0);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_pad_all(hdr, 8, 0);

  dd_profile = make_dropdown(hdr, "Theta Pico\nTheta\nSigma\nAlpha", 178, profile_cb);
  lv_obj_align(dd_profile, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_dropdown_set_selected(dd_profile, 0);

  lv_obj_t* dd_baud = make_dropdown(hdr, BAUD_OPTS, 112, baud_cb);
  lv_obj_align(dd_baud, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_dropdown_set_selected(dd_baud, 0);  // 9600 — Sensor_Terminal.py default
  hal_uart_set_baud(9600);

  lbl_status = lv_label_create(hdr);
  lv_label_set_text(lbl_status, "UART0 9600 8N1 — awaiting sensor…");
  lv_obj_set_style_text_color(lbl_status, GC_ON_SURFACE_VAR, 0);
  lv_obj_set_width(lbl_status, LV_PCT(100));
  lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  // Content: scrollable column of groups
  col_groups = lv_obj_create(tab);
  lv_obj_set_size(col_groups, LV_PCT(100), TAB_H - HDR_H - TERM_H);
  lv_obj_align(col_groups, LV_ALIGN_TOP_MID, 0, HDR_H);
  lv_obj_set_style_bg_opa(col_groups, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col_groups, 0, 0);
  lv_obj_set_style_pad_all(col_groups, 12, 0);
  lv_obj_set_style_pad_row(col_groups, 6, 0);
  lv_obj_set_flex_flow(col_groups, LV_FLEX_FLOW_COLUMN);

  // Bottom terminal readout
  txt_term = lv_textarea_create(tab);
  lv_obj_set_size(txt_term, LV_PCT(100), TERM_H);
  lv_obj_align(txt_term, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(txt_term, lv_color_black(), 0);
  lv_obj_set_style_text_color(txt_term, lv_color_hex(0x00FF00), 0);
  lv_obj_set_style_border_width(txt_term, 0, 0);
  lv_obj_set_style_shadow_width(txt_term, 0, 0);
  lv_obj_set_style_pad_all(txt_term, 6, 0);
  lv_textarea_set_one_line(txt_term, false);
  lv_textarea_set_cursor_click_pos(txt_term, false);
  lv_textarea_set_text(txt_term, "");

  build_groups();
}

// ---- Tab 3: LAN Discovery (Phase 3) -----------------------------------------
static lv_obj_t* lan_status;
static lv_obj_t* lan_col;
static uint32_t  lan_seen_gen = 0xFFFFFFFF;
// Static device cache: click callbacks index into it (cards are rebuilt on
// every generation bump, so user_data can't point at rebuild-locals).
static LanDevice lan_devs[16];
static uint8_t   lan_devs_n = 0;

static void lan_card_cb(lv_event_t* e) {
  uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= lan_devs_n) return;
  // Only application-mode Taus run the HTTP config server; the bootloader
  // (AN1388) and this unit itself are not configurable targets.
  if (strcmp(lan_devs[idx].state, "app") != 0) return;
  if (lan_devs[idx].sl_port == 0) return;   // self
  ui_tau_config_open(lan_devs[idx].ip);
}

static void lan_rebuild() {
  lv_obj_clean(lan_col);
  lan_devs_n = net_snapshot(lan_devs, 16);
  uint8_t n = lan_devs_n;
  LanDevice* devs = lan_devs;

  if (n == 0) {
    lv_obj_t* l = lv_label_create(lan_col);
    lv_label_set_text(l, net_connected()
        ? "No replies yet — probing…"
        : "Wi-Fi not connected");
    lv_obj_set_style_text_color(l, GC_ON_SURFACE_VAR, 0);
    return;
  }
  for (uint8_t i = 0; i < n; i++) {
    lv_obj_t* card = lv_obj_create(lan_col);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, GC_SURFACE, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // Application-state Taus open the configurator on tap.
    if (strcmp(devs[i].state, "app") == 0 && devs[i].sl_port > 0) {
      lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(card, lan_card_cb, LV_EVENT_CLICKED,
                          (void*)(uintptr_t)i);
      lv_obj_set_style_bg_color(card, GC_SURFACE_CONT, LV_STATE_PRESSED);
    }

    lv_obj_t* host = lv_label_create(card);
    lv_label_set_text(host, devs[i].host);
    lv_obj_set_style_text_color(host, GC_PRIMARY, 0);

    lv_obj_t* sub = lv_label_create(card);
    if (strcmp(devs[i].state, "boot") == 0) {
      lv_label_set_text_fmt(sub, "%s  •  BOOTLOADER", devs[i].ip);
    } else if (devs[i].sl_port > 0) {
      lv_label_set_text_fmt(sub, "%s  •  %s  •  SL :%u",
                            devs[i].ip, devs[i].mac, devs[i].sl_port);
    } else {
      lv_label_set_text_fmt(sub, "%s  •  %s  (this unit)",
                            devs[i].ip, devs[i].mac);
    }
    lv_obj_set_style_text_color(sub, GC_ON_SURFACE_VAR, 0);
  }
}

static void lan_refresh(lv_timer_t* t) {
  (void)t;
  if (!lan_status) return;
  lv_label_set_text_fmt(lan_status, "Wi-Fi: %s   IP: %s",
                        net_state_str(),
                        net_local_ip().c_str());
  if (net_generation() != lan_seen_gen) {
    lan_seen_gen = net_generation();
    lan_rebuild();
  }
}

static void lan_scan_cb(lv_event_t* e) {
  (void)e;
  audio_play(SOUND_PING_FAULT);   // auditory radar ping: NeuroFunk growl
  net_scan();                     // unblock the discovery task for one sweep
}

static void build_lan_tab(lv_obj_t* tab) {
  lan_status = lv_label_create(tab);
  lv_label_set_text(lan_status, "Wi-Fi: connecting…");
  lv_obj_set_style_text_color(lan_status, GC_ON_SURFACE_VAR, 0);
  lv_obj_set_style_pad_all(lan_status, 8, 0);
  lv_obj_align(lan_status, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t* scan = lv_btn_create(tab);
  lv_obj_set_size(scan, 120, 30);
  lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, -6, 2);
  lv_obj_set_style_bg_color(scan, GC_SURFACE_CONT, 0);
  lv_obj_set_style_radius(scan, 8, 0);
  lv_obj_add_event_cb(scan, lan_scan_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* sl = lv_label_create(scan);
  lv_label_set_text(sl, "SCAN FOR TAUS");
  lv_obj_set_style_text_color(sl, GC_PRIMARY, 0);
  lv_obj_center(sl);

  lan_col = lv_obj_create(tab);
  lv_obj_set_size(lan_col, LV_PCT(100), TAB_H - 36);
  lv_obj_align(lan_col, LV_ALIGN_TOP_MID, 0, 36);
  lv_obj_set_style_bg_opa(lan_col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(lan_col, 0, 0);
  lv_obj_set_style_pad_all(lan_col, 8, 0);
  lv_obj_set_style_pad_row(lan_col, 8, 0);
  lv_obj_set_flex_flow(lan_col, LV_FLEX_FLOW_COLUMN);

  lan_rebuild();
}

// ---- Tab 2: GNSS Radar (Phase 4) ---------------------------------------------
static const uint8_t GNSS_BARS = 6;
static lv_obj_t* gnss_status;
static lv_obj_t* gnss_pos;
static lv_obj_t* gnss_led;
static lv_obj_t* gnss_bars[GNSS_BARS];
static lv_obj_t* gnss_bar_labels[GNSS_BARS];
static uint32_t  gnss_seen_gen = 0xFFFFFFFF;
static uint32_t  gnss_seen_pps = 0;
static uint32_t  gnss_led_off_at = 0;
static lv_obj_t* gnss_console;
static uint32_t  gnss_seen_raw = 0xFFFFFFFF;

static const char* fix_str(uint8_t q) {
  switch (q) {
    case 1:  return "GPS Fix";
    case 2:  return "DGPS Fix";
    case 4:  return "RTK Fix";
    case 5:  return "RTK Float";
    default: return "No Fix";
  }
}

static void gnss_refresh(lv_timer_t* t) {
  (void)t;
  // 1 PPS lamp: light on every new ISR edge, hold ~150 ms, then off.
  uint32_t pps = gnss_pps_count();
  if (pps != gnss_seen_pps) {
    gnss_seen_pps = pps;
    lv_led_on(gnss_led);
    gnss_led_off_at = millis() + 150;
  } else if (gnss_led_off_at && (int32_t)(millis() - gnss_led_off_at) > 0) {
    lv_led_off(gnss_led);
    gnss_led_off_at = 0;
  }

  // Raw console: repaint on any new sentence (raw gen bumps for every NMEA
  // line, including VTG/GLL/PSTMCPU that the parsed-state generation ignores).
  if (gnss_console && gnss_raw_generation() != gnss_seen_raw) {
    gnss_seen_raw = gnss_raw_generation();
    char buf[420];
    gnss_raw_snapshot(buf, sizeof(buf));
    lv_label_set_text(gnss_console, buf[0] ? buf : "(awaiting NMEA)");
  }

  if (!gnss_status || gnss_generation() == gnss_seen_gen) return;
  gnss_seen_gen = gnss_generation();

  GnssState g;
  gnss_snapshot(&g);
  const char* gsa_dim = g.gsa_fix == 3 ? "3D" : g.gsa_fix == 2 ? "2D" : "--";
  lv_label_set_text_fmt(gnss_status, "%s   SV %u/%u   GSA %c:%s",
                        fix_str(g.fix_quality), g.sats_used, g.sats_in_view,
                        g.gsa_mode ? g.gsa_mode : '-', gsa_dim);
  lv_obj_set_style_text_color(gnss_status,
                              g.locked ? GC_PRIMARY : GC_ON_SURFACE_VAR, 0);
  if (g.locked) {
    // Pre-format with newlib snprintf: full %f fidelity regardless of LVGL's
    // built-in vsnprintf coverage, and no varargs handed to LVGL at all.
    char pos[120];
    snprintf(pos, sizeof(pos),
             "%.5f, %.5f   alt %.1f m\n%s UTC   %.1f kn   crs %.0f deg   HDOP %.1f",
             g.lat, g.lon, (double)g.alt_m,
             g.utc[0] ? g.utc : "--",
             (double)g.speed_kn, (double)g.course_deg, (double)g.hdop);
    lv_label_set_text(gnss_pos, pos);
  } else {
    lv_label_set_text_fmt(gnss_pos, "GGA %lu  RMC %lu  GSA %lu (%s)  GSV %lu",
                          (unsigned long)g.gga_count,
                          (unsigned long)g.rmc_count,
                          (unsigned long)g.gsa_count, gsa_dim,
                          (unsigned long)g.gsv_cycles);
  }

  GnssSat sats[GNSS_BARS];
  uint8_t n = gnss_top_sats(sats, GNSS_BARS);
  for (uint8_t i = 0; i < GNSS_BARS; i++) {
    if (i < n) {
      lv_bar_set_value(gnss_bars[i], sats[i].snr, LV_ANIM_OFF);
      lv_label_set_text_fmt(gnss_bar_labels[i], "SV%u  %u", sats[i].prn, sats[i].snr);
      lv_obj_clear_flag(gnss_bars[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(gnss_bar_labels[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(gnss_bars[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(gnss_bar_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void build_gnss_tab(lv_obj_t* tab) {
  gnss_status = lv_label_create(tab);
  lv_label_set_text(gnss_status, "No Fix   SV 0/0");
  lv_obj_set_style_text_color(gnss_status, GC_ON_SURFACE_VAR, 0);
  lv_obj_align(gnss_status, LV_ALIGN_TOP_LEFT, 10, 10);

  // 1 PPS lamp (top-right) — flashes green on each ISR edge.
  gnss_led = lv_led_create(tab);
  lv_obj_set_size(gnss_led, 18, 18);
  lv_led_set_color(gnss_led, lv_color_hex(0x22C55E));
  lv_led_off(gnss_led);
  lv_obj_align(gnss_led, LV_ALIGN_TOP_RIGHT, -14, 8);
  lv_obj_t* pl = lv_label_create(tab);
  lv_label_set_text(pl, "1PPS");
  lv_obj_set_style_text_color(pl, GC_ON_SURFACE_VAR, 0);
  lv_obj_set_style_text_font(pl, &lv_font_montserrat_10, 0);
  lv_obj_align(pl, LV_ALIGN_TOP_RIGHT, -44, 12);

  gnss_pos = lv_label_create(tab);
  lv_label_set_text(gnss_pos, "awaiting NMEA on UART2 (RX=46)…");
  lv_obj_set_style_text_color(gnss_pos, GC_ON_SURFACE_VAR, 0);
  lv_obj_align(gnss_pos, LV_ALIGN_TOP_MID, 0, 32);

  // SNR bars: label above each bar, vertical bars in a row.
  lv_obj_t* row = lv_obj_create(tab);
  lv_obj_set_size(row, LV_PCT(100), TAB_H - 180);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 68);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  for (uint8_t i = 0; i < GNSS_BARS; i++) {
    lv_obj_t* cell = lv_obj_create(row);
    lv_obj_set_size(cell, 46, LV_PCT(100));
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    gnss_bars[i] = lv_bar_create(cell);
    lv_obj_set_size(gnss_bars[i], 22, LV_PCT(75));
    lv_obj_align(gnss_bars[i], LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(gnss_bars[i], 0, 50);        // SNR dB-Hz
    lv_obj_set_style_bg_color(gnss_bars[i], GC_SURFACE_CONT, 0);
    lv_obj_set_style_bg_color(gnss_bars[i], GC_PRIMARY, LV_PART_INDICATOR);

    gnss_bar_labels[i] = lv_label_create(cell);
    lv_label_set_text(gnss_bar_labels[i], "—");
    lv_obj_set_style_text_color(gnss_bar_labels[i], GC_ON_SURFACE_VAR, 0);
    lv_obj_align(gnss_bar_labels[i], LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(gnss_bar_labels[i], &lv_font_montserrat_10, 0);

    lv_obj_add_flag(gnss_bars[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(gnss_bar_labels[i], LV_OBJ_FLAG_HIDDEN);
  }

  // Raw NMEA console: last 4 sentences exactly as received on IO46.
  gnss_console = lv_label_create(tab);
  lv_label_set_text(gnss_console, "(awaiting NMEA)");
  lv_obj_set_style_text_color(gnss_console, GC_ON_SURFACE_VAR, 0);
  lv_obj_set_style_text_font(gnss_console, &lv_font_montserrat_10, 0);
  lv_label_set_long_mode(gnss_console, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(gnss_console, 300);
  lv_obj_align(gnss_console, LV_ALIGN_BOTTOM_LEFT, 10, -4);
}

// ---- Tab 4: ICSP Flasher foundation (Phase 5) ---------------------------------
static const uint8_t HEX_MAX = 16;
static lv_obj_t* icsp_list;
static lv_obj_t* icsp_status;
static lv_obj_t* icsp_dd_target;
static char      icsp_selected[40] = "";

static const char* TARGET_OPTS = "PIC32MZ\nPIC24F\nPIC16\nPIC18";

static void icsp_file_cb(lv_event_t* e) {
  const char* name = (const char*)lv_event_get_user_data(e);
  snprintf(icsp_selected, sizeof(icsp_selected), "%s", name);
  lv_label_set_text_fmt(icsp_status, "Selected: %s", icsp_selected);
}

static void icsp_rescan() {
  lv_obj_clean(icsp_list);
  icsp_selected[0] = '\0';

  if (!storage_mounted()) {
    lv_list_add_text(icsp_list, "SD card not mounted");
    lv_label_set_text(icsp_status, "Insert SD and rescan");
    return;
  }
  static char names[16][40];
  uint8_t n = storage_list_hex(names, HEX_MAX);
  if (n == 0) {
    lv_list_add_text(icsp_list, "/firmware is empty — copy .hex files here");
    lv_label_set_text(icsp_status, "No .hex files found");
    return;
  }
  for (uint8_t i = 0; i < n; i++) {
    lv_obj_t* b = lv_list_add_btn(icsp_list, LV_SYMBOL_FILE, names[i]);
    lv_obj_set_style_bg_color(b, GC_SURFACE, 0);
    lv_obj_set_style_bg_color(b, GC_SURFACE_CONT, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, GC_ON_SURFACE, 0);
    // names[] is static: safe to hand the pointer to the event
    lv_obj_add_event_cb(b, icsp_file_cb, LV_EVENT_CLICKED, names[i]);
  }
  lv_label_set_text_fmt(icsp_status, "%u .hex file(s) — select one", n);
}

static void icsp_rescan_cb(lv_event_t* e) { (void)e; icsp_rescan(); }

static void icsp_flash_cb(lv_event_t* e) {
  (void)e;
  audio_play(SOUND_ACTION_KICK);   // primary action: HardTek kick
  if (icsp_selected[0] == '\0') {
    lv_label_set_text(icsp_status, "Select a .hex file first");
    return;
  }
  char target[12];
  lv_dropdown_get_selected_str(icsp_dd_target, target, sizeof(target));
  // Phase 5 scope: binding only — no LVP protocol yet.
  lv_label_set_text_fmt(icsp_status, "ARMED: %s -> %s (protocol pending)",
                        icsp_selected, target);
}

static void build_icsp_tab(lv_obj_t* tab) {
  // Header: target dropdown + rescan button
  icsp_dd_target = make_dropdown(tab, TARGET_OPTS, 150, NULL);
  lv_obj_align(icsp_dd_target, LV_ALIGN_TOP_LEFT, 8, 8);

  lv_obj_t* rescan = lv_btn_create(tab);
  lv_obj_set_size(rescan, 110, 36);
  lv_obj_align(rescan, LV_ALIGN_TOP_RIGHT, -8, 8);
  lv_obj_set_style_bg_color(rescan, GC_SURFACE_CONT, 0);
  lv_obj_set_style_radius(rescan, 8, 0);
  lv_obj_add_event_cb(rescan, icsp_rescan_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* rl = lv_label_create(rescan);
  lv_label_set_text(rl, "RESCAN");
  lv_obj_set_style_text_color(rl, GC_ON_SURFACE, 0);
  lv_obj_center(rl);

  // File list
  icsp_list = lv_list_create(tab);
  lv_obj_set_size(icsp_list, LV_PCT(100), TAB_H - 56 - 96);
  lv_obj_align(icsp_list, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_set_style_bg_color(icsp_list, GC_BACKGROUND, 0);
  lv_obj_set_style_border_width(icsp_list, 0, 0);

  // FLASH TARGET button
  lv_obj_t* flash = lv_btn_create(tab);
  lv_obj_set_size(flash, LV_PCT(92), 44);
  lv_obj_align(flash, LV_ALIGN_BOTTOM_MID, 0, -44);
  lv_obj_set_style_bg_color(flash, GC_PRIMARY, 0);
  lv_obj_set_style_bg_color(flash, lv_color_hex(0x11C6CC), LV_STATE_PRESSED);
  lv_obj_set_style_radius(flash, 10, 0);
  lv_obj_add_event_cb(flash, icsp_flash_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* fl = lv_label_create(flash);
  lv_label_set_text(fl, "FLASH TARGET");
  lv_obj_set_style_text_color(fl, GC_ON_PRIMARY, 0);
  lv_obj_center(fl);

  // Status line
  icsp_status = lv_label_create(tab);
  lv_label_set_text(icsp_status, "—");
  lv_obj_set_style_text_color(icsp_status, GC_ON_SURFACE_VAR, 0);
  lv_obj_set_width(icsp_status, LV_PCT(100));
  lv_label_set_long_mode(icsp_status, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_align(icsp_status, LV_ALIGN_BOTTOM_MID, 0, -14);

  icsp_rescan();
}

// ---- Main tabbed screen ------------------------------------------------------
static void style_tab_bar(lv_obj_t* tv) {
  lv_obj_t* btns = lv_tabview_get_tab_btns(tv);
  lv_obj_set_style_bg_color(btns, GC_SURFACE, 0);
  lv_obj_set_style_border_width(btns, 0, 0);
  lv_obj_set_style_pad_all(btns, 0, 0);
  // Inactive tabs: muted text on surface. Active tab: teal text + teal
  // bottom border as the MD3 indicator.
  lv_obj_set_style_text_color(btns, GC_ON_SURFACE_VAR, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(btns, GC_SURFACE, LV_PART_ITEMS);
  lv_obj_set_style_text_color(btns, GC_PRIMARY,
                              LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(btns, GC_PRIMARY,
                                LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_side(btns, LV_BORDER_SIDE_BOTTOM,
                               LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(btns, 3, LV_PART_ITEMS | LV_STATE_CHECKED);
}

static void build_tabs() {
  scr_main = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_main, GC_BACKGROUND, 0);
  lv_obj_set_style_pad_all(scr_main, 0, 0);

  tabview = lv_tabview_create(scr_main, LV_DIR_TOP, TAB_BAR_H);
  lv_obj_set_size(tabview, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(tabview, GC_BACKGROUND, 0);
  style_tab_bar(tabview);
  lv_obj_add_event_cb(tabview, nav_zap_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t* tab_sensor = lv_tabview_add_tab(tabview, "Sensor");
  lv_obj_t* tab_gnss   = lv_tabview_add_tab(tabview, "GNSS");
  lv_obj_t* tab_lan    = lv_tabview_add_tab(tabview, "LAN");
  lv_obj_t* tab_seed   = lv_tabview_add_tab(tabview, "SEED");
  lv_obj_t* tab_icsp   = lv_tabview_add_tab(tabview, "ICSP");

  // Strip default page chrome on every tab.
  lv_obj_t* tabs[] = {tab_sensor, tab_gnss, tab_lan, tab_seed, tab_icsp};
  for (uint8_t i = 0; i < 5; i++) {
    lv_obj_set_style_bg_opa(tabs[i], LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(tabs[i], 0, 0);
    lv_obj_clear_flag(tabs[i], LV_OBJ_FLAG_SCROLLABLE); // inner cols scroll
  }

  build_sensor_tab(tab_sensor);
  build_gnss_tab(tab_gnss);
  build_lan_tab(tab_lan);
  ui_seed_build(tab_seed);
  build_icsp_tab(tab_icsp);

  lv_timer_create(lan_refresh, 500, NULL);
  lv_timer_create(gnss_refresh, 100, NULL);
  lv_scr_load(scr_main);
}

static void splash_timer(lv_timer_t* t) {
  build_tabs();
  lv_timer_del(t);
}

// Return from a sub-screen (Tau Configurator) to the main tabview.
void ui_show_main() {
  if (scr_main) lv_scr_load(scr_main);
}

// ---- Splash ------------------------------------------------------------------
void ui_init() {
  scr_splash = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_splash, GC_BACKGROUND, 0);

  lv_obj_t* img = lv_img_create(scr_splash);
  lv_img_set_src(img, &gaiacode_logo);
  lv_obj_align(img, LV_ALIGN_CENTER, 0, -30);

  lv_obj_t* sub = lv_label_create(scr_splash);
  lv_label_set_text(sub, "Universal Sensor Terminal");
  lv_obj_set_style_text_color(sub, GC_ON_SURFACE_VAR, 0);
  lv_obj_align(sub, LV_ALIGN_CENTER, 0, 70);

  lv_scr_load(scr_splash);

  lv_timer_create(splash_timer, 2500, NULL);
  lv_timer_create(rx_timer, 50, NULL);
}
