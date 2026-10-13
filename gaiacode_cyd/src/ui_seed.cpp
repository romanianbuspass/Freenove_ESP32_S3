#include "ui_seed.h"
#include "slink.h"
#include "net.h"
#include "audio.h"
#include <string.h>
#include <stdlib.h>

// ---- MD3 tokens (mirror ui.cpp) --------------------------------------------
#define GC_PRIMARY        lv_color_hex(0x0EADB2)
#define GC_ON_PRIMARY     lv_color_hex(0x0F172A)
#define GC_SURFACE        lv_color_hex(0x1E293B)
#define GC_SURFACE_CONT   lv_color_hex(0x263245)
#define GC_ON_SURFACE     lv_color_hex(0xE2E8F0)
#define GC_ON_SURFACE_VAR lv_color_hex(0x94A3B8)
#define GC_DANGER         lv_color_hex(0xB34040)

static const lv_coord_t TRACE_W   = 296;   // trace width in px
static const lv_coord_t LANE_H    = 78;    // lane container height
static const lv_coord_t TRACE_H   = 56;    // trace height inside a lane
static const uint16_t   PTS       = 148;   // polyline points per trace

static const uint16_t WINDOW_S[4] = {10, 30, 60, 300};

// ---- Widgets --------------------------------------------------------------------
static lv_obj_t* s_dd_tau;
static lv_obj_t* s_btn_conn;
static lv_obj_t* s_lbl_conn;
static lv_obj_t* s_ta_sel;
static lv_obj_t* s_dd_win;
static lv_obj_t* s_status;
static lv_obj_t* s_kb;
static lv_obj_t* s_lanes[SLINK_MAX_LANES];
static lv_obj_t* s_lane_lbl[SLINK_MAX_LANES];
static lv_obj_t* s_trace[SLINK_MAX_LANES];
static lv_point_t s_pts[SLINK_MAX_LANES][PTS];

// Tau dropdown backing data (app-state devices from the LAN scanner)
static char     s_tau_ip[16][16];
static uint16_t s_tau_port[16];
static uint8_t  s_tau_n = 0;
static uint32_t s_seen_net_gen = 0xFFFFFFFF;
static uint32_t s_seen_lane_gen = 0xFFFFFFFF;

// ---- Keyboard ----------------------------------------------------------------------
static void sel_focus_cb(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb, s_ta_sel);
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
  } else {
    lv_keyboard_set_textarea(s_kb, NULL);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  }
}
static void kb_done_cb(lv_event_t* e) {
  (void)e;
  lv_keyboard_set_textarea(s_kb, NULL);
  lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

// ---- Tau picker -----------------------------------------------------------------------
static void tau_dropdown_refresh() {
  LanDevice devs[16];
  uint8_t n = net_snapshot(devs, 16);
  s_tau_n = 0;
  String opts = "";
  for (uint8_t i = 0; i < n; i++) {
    if (strcmp(devs[i].state, "app") != 0 || devs[i].sl_port == 0) continue;
    snprintf(s_tau_ip[s_tau_n], 16, "%s", devs[i].ip);
    s_tau_port[s_tau_n] = devs[i].sl_port;
    if (s_tau_n) opts += "\n";
    opts += String(devs[i].host) + " " + devs[i].ip;
    s_tau_n++;
    if (s_tau_n >= 16) break;
  }
  if (!s_tau_n) opts = "(no Taus — run LAN scan)";
  lv_dropdown_set_options(s_dd_tau, opts.c_str());
}

// ---- Connect / disconnect -----------------------------------------------------------------
static void conn_cb(lv_event_t* e) {
  (void)e;
  const char* st = slink_state();
  if (strcmp(st, "streaming") == 0 || strcmp(st, "connecting") == 0) {
    slink_disconnect();
    return;
  }
  if (!s_tau_n) { lv_label_set_text(s_status, "No Tau selected — run LAN scan first."); return; }
  uint16_t sel = lv_dropdown_get_selected(s_dd_tau);
  if (sel >= s_tau_n) sel = 0;
  const char* pattern = lv_textarea_get_text(s_ta_sel);
  if (!pattern[0]) pattern = "XX.????.??.F??";
  audio_play(SOUND_ACTION_KICK);
  slink_connect(s_tau_ip[sel], s_tau_port[sel], pattern);
}

// ---- Trace redraw ---------------------------------------------------------------------------
static int32_t* s_buf = NULL;   // decimation buffer, allocated in PSRAM at build

static void redraw_lane(uint8_t lane) {
  if (!s_buf) return;
  int32_t* buf = s_buf;
  SlinkLaneInfo info[SLINK_MAX_LANES];
  if (lane >= slink_lanes(info, SLINK_MAX_LANES)) return;
  float sps = info[lane].sps;

  uint16_t win = WINDOW_S[lv_dropdown_get_selected(s_dd_win)];
  uint32_t need = (sps > 0.01f) ? (uint32_t)(sps * win) : SLINK_RING;
  if (need < 2) need = 2;
  if (need > SLINK_RING) need = SLINK_RING;
  uint32_t n = slink_read_lane(lane, buf, need, NULL);
  if (n < 2) {
    lv_obj_add_flag(s_trace[lane], LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(s_trace[lane], LV_OBJ_FLAG_HIDDEN);

  // Decimate to PTS means + window peak for scaling.
  int32_t peak = 1;
  for (uint32_t i = 0; i < n; i++) {
    int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
    if (a > peak) peak = a;
  }
  lv_coord_t mid = TRACE_H / 2;
  lv_coord_t amp = mid - 3;
  for (uint16_t p = 0; p < PTS; p++) {
    uint32_t lo = (uint32_t)p * n / PTS;
    uint32_t hi = (uint32_t)(p + 1) * n / PTS;
    if (hi <= lo) hi = lo + 1;
    int64_t acc = 0;
    for (uint32_t i = lo; i < hi; i++) acc += buf[i];
    int32_t mean = (int32_t)(acc / (int64_t)(hi - lo));
    s_pts[lane][p].x = (lv_coord_t)((uint32_t)p * (TRACE_W - 1) / (PTS - 1));
    s_pts[lane][p].y = mid - (lv_coord_t)((int64_t)mean * amp / peak);
  }
  lv_line_set_points(s_trace[lane], s_pts[lane], PTS);

  lv_label_set_text_fmt(s_lane_lbl[lane], "%s  %.1f sps  pk %ld",
                        info[lane].id, info[lane].sps, (long)peak);
}

// ---- Poll timer ----------------------------------------------------------------------------------
static void seed_poll(lv_timer_t* t) {
  (void)t;
  if (!s_dd_tau) return;

  // Tau picker follows the LAN scanner until a stream is active.
  const char* st = slink_state();
  bool active = strcmp(st, "streaming") == 0 || strcmp(st, "connecting") == 0;
  if (!active && net_generation() != s_seen_net_gen) {
    s_seen_net_gen = net_generation();
    tau_dropdown_refresh();
  }

  lv_label_set_text(s_lbl_conn, active ? "DISCONNECT" : "CONNECT");

  if (strcmp(st, "error") == 0) {
    lv_label_set_text_fmt(s_status, "ERROR: %s", slink_error());
    lv_obj_set_style_text_color(s_status, GC_DANGER, 0);
  } else {
    uint8_t ovf = slink_overflow();
    lv_label_set_text_fmt(s_status, "%s  rec %lu  matched %lu%s",
                          st,
                          (unsigned long)slink_records(),
                          (unsigned long)slink_matched(),
                          ovf ? "  (+more streams)" : "");
    lv_obj_set_style_text_color(s_status, GC_ON_SURFACE_VAR, 0);
  }

  // Lane objects follow the matched-stream set.
  if (slink_generation() != s_seen_lane_gen) {
    s_seen_lane_gen = slink_generation();
    SlinkLaneInfo info[SLINK_MAX_LANES];
    uint8_t n = slink_lanes(info, SLINK_MAX_LANES);
    for (uint8_t i = 0; i < SLINK_MAX_LANES; i++) {
      if (i < n) {
        lv_obj_clear_flag(s_lanes[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(s_lane_lbl[i], "%s  %.1f sps", info[i].id, info[i].sps);
      } else {
        lv_obj_add_flag(s_lanes[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
  }

  if (active) {
    for (uint8_t i = 0; i < SLINK_MAX_LANES; i++)
      if (!lv_obj_has_flag(s_lanes[i], LV_OBJ_FLAG_HIDDEN)) redraw_lane(i);
  }
}

// ---- Build -----------------------------------------------------------------------------------------
void ui_seed_build(lv_obj_t* tab) {
  // Decimation buffer: CPU-only, PSRAM-placed (keeps internal SRAM for the heap)
  s_buf = (int32_t*)ps_malloc(SLINK_RING * sizeof(int32_t));
  if (!s_buf) s_buf = (int32_t*)malloc(SLINK_RING * sizeof(int32_t));
  if (!s_buf) Serial.println("SEED: !! decimation buffer allocation failed");

  // Row 1: Tau picker + connect button
  s_dd_tau = lv_dropdown_create(tab);
  lv_obj_set_size(s_dd_tau, 172, 32);
  lv_obj_align(s_dd_tau, LV_ALIGN_TOP_LEFT, 4, 4);
  lv_obj_set_style_bg_color(s_dd_tau, GC_SURFACE_CONT, 0);
  lv_obj_set_style_text_color(s_dd_tau, GC_ON_SURFACE, 0);

  s_btn_conn = lv_btn_create(tab);
  lv_obj_set_size(s_btn_conn, 118, 32);
  lv_obj_align(s_btn_conn, LV_ALIGN_TOP_RIGHT, -4, 4);
  lv_obj_set_style_bg_color(s_btn_conn, GC_SURFACE_CONT, 0);
  lv_obj_set_style_radius(s_btn_conn, 8, 0);
  lv_obj_add_event_cb(s_btn_conn, conn_cb, LV_EVENT_CLICKED, NULL);
  s_lbl_conn = lv_label_create(s_btn_conn);
  lv_label_set_text(s_lbl_conn, "CONNECT");
  lv_obj_set_style_text_color(s_lbl_conn, GC_PRIMARY, 0);
  lv_obj_center(s_lbl_conn);

  // Row 2: selector + window
  s_ta_sel = lv_textarea_create(tab);
  lv_textarea_set_one_line(s_ta_sel, true);
  lv_textarea_set_max_length(s_ta_sel, SLINK_PATTERN_LEN - 1);
  lv_textarea_set_text(s_ta_sel, slink_pattern());
  lv_obj_set_size(s_ta_sel, 200, 34);
  lv_obj_align(s_ta_sel, LV_ALIGN_TOP_LEFT, 4, 42);
  lv_obj_set_style_bg_color(s_ta_sel, GC_SURFACE_CONT, 0);
  lv_obj_set_style_text_color(s_ta_sel, GC_ON_SURFACE, 0);
  lv_obj_add_event_cb(s_ta_sel, sel_focus_cb, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb(s_ta_sel, sel_focus_cb, LV_EVENT_DEFOCUSED, NULL);

  s_dd_win = lv_dropdown_create(tab);
  lv_dropdown_set_options(s_dd_win, "10 s\n30 s\n60 s\n5 min");
  lv_dropdown_set_selected(s_dd_win, 2);
  lv_obj_set_size(s_dd_win, 96, 34);
  lv_obj_align(s_dd_win, LV_ALIGN_TOP_RIGHT, -4, 42);
  lv_obj_set_style_bg_color(s_dd_win, GC_SURFACE_CONT, 0);
  lv_obj_set_style_text_color(s_dd_win, GC_ON_SURFACE, 0);

  // Status line (word-wrapped: error detail must not clip off-panel)
  s_status = lv_label_create(tab);
  lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_status, 308);
  lv_label_set_text(s_status, "idle — pick a Tau and CONNECT");
  lv_obj_set_style_text_color(s_status, GC_ON_SURFACE_VAR, 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 6, 82);

  // Lanes
  for (uint8_t i = 0; i < SLINK_MAX_LANES; i++) {
    lv_obj_t* lane = lv_obj_create(tab);
    lv_obj_set_size(lane, 312, LANE_H);
    lv_obj_align(lane, LV_ALIGN_TOP_LEFT, 4, 114 + i * (LANE_H + 4));
    lv_obj_set_style_bg_color(lane, GC_SURFACE, 0);
    lv_obj_set_style_radius(lane, 6, 0);
    lv_obj_set_style_border_width(lane, 0, 0);
    lv_obj_set_style_pad_all(lane, 4, 0);
    lv_obj_clear_flag(lane, LV_OBJ_FLAG_SCROLLABLE);

    s_lane_lbl[i] = lv_label_create(lane);
    lv_label_set_text(s_lane_lbl[i], "-");
    lv_obj_set_style_text_color(s_lane_lbl[i], GC_PRIMARY, 0);
    lv_obj_align(s_lane_lbl[i], LV_ALIGN_TOP_LEFT, 2, 0);

    lv_obj_t* frame = lv_obj_create(lane);
    lv_obj_set_size(frame, TRACE_W + 4, TRACE_H + 4);
    lv_obj_align(frame, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_radius(frame, 4, 0);
    lv_obj_set_style_border_width(frame, 0, 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

    s_trace[i] = lv_line_create(frame);
    lv_obj_set_style_line_color(s_trace[i], GC_PRIMARY, 0);
    lv_obj_set_style_line_width(s_trace[i], 1, 0);
    for (uint16_t p = 0; p < PTS; p++) {
      s_pts[i][p].x = (lv_coord_t)((uint32_t)p * (TRACE_W - 1) / (PTS - 1));
      s_pts[i][p].y = TRACE_H / 2;
    }
    lv_line_set_points(s_trace[i], s_pts[i], PTS);
    lv_obj_set_pos(s_trace[i], 2, 2);

    lv_obj_add_flag(lane, LV_OBJ_FLAG_HIDDEN);
    s_lanes[i] = lane;
  }

  // Shared keyboard (hidden until the selector is focused)
  s_kb = lv_keyboard_create(tab);
  lv_obj_set_size(s_kb, 320, 180);
  lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_kb, kb_done_cb, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(s_kb, kb_done_cb, LV_EVENT_CANCEL, NULL);

  tau_dropdown_refresh();
  lv_timer_create(seed_poll, 500, NULL);
}
