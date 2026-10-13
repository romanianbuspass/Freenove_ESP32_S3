#include "ui_tau_config.h"
#include "ui.h"
#include "tau_config.h"
#include "audio.h"
#include <lvgl.h>
#include <string.h>
#include <stdlib.h>

// GaiaCode CYD — Tau Configurator screen.
// Single-scroll layout per project convention: Overview table first, then
// grouped operational sections, destructive controls isolated at the end.
// Command builders mirror TauConfigurator.svelte verbatim (filters.htm only —
// the desktop's Sensor.htm path is currently unused there too).

// ---- MD3 tokens (mirror ui.cpp) --------------------------------------------
#define GC_PRIMARY        lv_color_hex(0x0EADB2)
#define GC_ON_PRIMARY     lv_color_hex(0x0F172A)
#define GC_SURFACE        lv_color_hex(0x1E293B)
#define GC_SURFACE_CONT   lv_color_hex(0x263245)
#define GC_ON_SURFACE     lv_color_hex(0xE2E8F0)
#define GC_ON_SURFACE_VAR lv_color_hex(0x94A3B8)
#define GC_DANGER         lv_color_hex(0xB34040)

// ---- Desktop grammar constants ----------------------------------------------
static const uint16_t SAMPLE_RATES[16] = {1000,500,250,200,125,100,50,40,25,20,10,8,5,4,2,1};
static const uint16_t AUX_SPS[13]      = {1000,500,250,200,125,100,50,40,20,10,5,2,1};
static const char*    AUX_LABELS[8]    = {"Pwr","PGA","T/H","SOH","MEMS","M1","M2","M3"};
static const uint8_t  GROUP_A[4] = {1,2,3,4};
static const uint8_t  GROUP_B[4] = {5,6,7,8};

static const char* FACTORY_RESET_CMDS[] = {
  "255 255 255 255 ip", "0 18000 port", "255 255 255 255 gateway",
  "255 255 255 0 mask", "255 255 255 255 ntp", "255 255 255 255 dns",
  "100 1 ch 100 2 ch 100 3 ch 100 4 ch", "100 5 ch 100 6 ch 100 7 ch 100 8 ch",
  "1 aux", "0 gpsx", "0 tcpflush", "0 dissd", "0 sdmode", "0 trigger",
  "1 cperg", "10 lograte", "10 lograteh", "^yyyyyyyn statusmode",
  "^nnnnnnnn statmodeh", "0 hptype", "0 bptype", "0 hpmask", "0 bpmask", "seed",
};

// ---- Screen state -------------------------------------------------------------
static lv_obj_t* s_scr = NULL;
static lv_obj_t* s_col;          // scrollable sections column
static lv_obj_t* s_lbl_ip;
static lv_obj_t* s_lbl_status;   // worker state / form errors
static lv_obj_t* s_kb;           // shared keyboard
static char      s_ip[16] = "";

// Overview
static lv_obj_t* s_ov_col;
static uint32_t  s_ov_seen_gen = 0xFFFFFFFF;

// Results
static lv_obj_t* s_res_col;
static uint32_t  s_res_seen_gen = 0xFFFFFFFF;
static lv_timer_t* s_poll = NULL;

// Confirm modal
static lv_obj_t* s_modal = NULL;
static lv_obj_t* s_modal_title;
static lv_obj_t* s_modal_cmds;
static lv_obj_t* s_modal_phrase_ta;
static lv_obj_t* s_modal_phrase_lbl;
static char     s_pending[TAU_MAX_CMDS][TAU_CMD_LEN];
static uint8_t  s_pending_n = 0;
static char     s_phrase[20] = "";   // "" = no typed phrase required

// ---- Form state (mirrors desktop) ----------------------------------------------
// Sample rate
static uint8_t  sr_group = 0;        // 0=A 1=B 2=ALL
static bool     sr_sel[16];
static lv_obj_t* sr_chips[16];
static lv_obj_t* sr_group_btns[3];
static lv_obj_t* sr_preview;
// PGA
static lv_obj_t* dd_pga_chan;
static lv_obj_t* dd_pga_gain;
// Network
static lv_obj_t *ta_port, *ta_gw, *ta_mask, *ta_dns, *ta_ntp;
static lv_obj_t* dd_llsps;
// Protocol
static uint8_t  seed_mode = 1;       // 1/2/8
static lv_obj_t* seed_mode_btns[3];
static lv_obj_t* ta_locs[8];
static lv_obj_t* ta_streams[8];
static lv_obj_t *ta_seedsta, *ta_seednet;
// Aux
static lv_obj_t *dd_maxtx, *dd_aux_llsps, *dd_tcpflush, *dd_emode;
static lv_obj_t *ta_lograte, *ta_lograteh;
static lv_obj_t* cb_sm[8];
static lv_obj_t* cb_smh[8];
// Trigger
static lv_obj_t *ta_pre, *ta_post;
static lv_obj_t* cb_hp[8];
static lv_obj_t *dd_hpsps, *dd_hptype;
static lv_obj_t* ta_lvl;
static lv_obj_t* cb_lvl_all;
static lv_obj_t* cb_bp[8];
static lv_obj_t *dd_bpsps, *dd_bptype;
static lv_obj_t *ta_sta, *ta_lta, *ta_rat;
static lv_obj_t *cb_sta_all, *cb_lta_all, *cb_rat_all;
// Rotation
static lv_obj_t *ta_rot, *ta_rotm;

// ---- Small helpers ----------------------------------------------------------------
static lv_obj_t* mk_card(lv_obj_t* parent, const char* title, bool danger) {
  lv_obj_t* card = lv_obj_create(parent);
  lv_obj_set_width(card, LV_PCT(100));
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(card, GC_SURFACE, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_border_width(card, danger ? 2 : 0, 0);
  lv_obj_set_style_border_color(card, GC_DANGER, 0);
  lv_obj_set_style_pad_all(card, 8, 0);
  lv_obj_set_style_pad_row(card, 6, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  if (title) {
    lv_obj_t* t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, danger ? GC_DANGER : GC_PRIMARY, 0);
  }
  return card;
}

static lv_obj_t* mk_row(lv_obj_t* parent) {
  lv_obj_t* r = lv_obj_create(parent);
  lv_obj_set_width(r, LV_PCT(100));
  lv_obj_set_height(r, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(r, 0, 0);
  lv_obj_set_style_pad_all(r, 0, 0);
  lv_obj_set_style_pad_column(r, 6, 0);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  return r;
}

static lv_obj_t* mk_btn(lv_obj_t* parent, const char* text, lv_event_cb_t cb,
                        bool danger) {
  lv_obj_t* b = lv_btn_create(parent);
  lv_obj_set_height(b, 34);
  lv_obj_set_style_bg_color(b, danger ? GC_DANGER : GC_SURFACE_CONT, 0);
  lv_obj_set_style_radius(b, 8, 0);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, danger ? GC_ON_SURFACE : GC_PRIMARY, 0);
  lv_obj_center(l);
  return b;
}

static void set_err(const char* msg) {
  lv_label_set_text(s_lbl_status, msg);
  lv_obj_set_style_text_color(s_lbl_status, GC_DANGER, 0);
}

// ---- Shared keyboard -------------------------------------------------------------
static void ta_focus_cb(lv_event_t* e) {
  lv_obj_t* ta = lv_event_get_target(e);
  if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
    bool numeric = (lv_obj_get_user_data(ta) != NULL);
    lv_keyboard_set_mode(s_kb, numeric ? LV_KEYBOARD_MODE_NUMBER
                                       : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
  } else if (lv_event_get_code(e) == LV_EVENT_DEFOCUSED) {
    lv_keyboard_set_textarea(s_kb, NULL);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  }
}

static void kb_event_cb(lv_event_t* e) {
  (void)e;
  lv_keyboard_set_textarea(s_kb, NULL);
  lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t* mk_ta(lv_obj_t* parent, const char* placeholder, uint16_t maxlen,
                       bool numeric, lv_coord_t w) {
  lv_obj_t* ta = lv_textarea_create(parent);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_max_length(ta, maxlen);
  lv_textarea_set_placeholder_text(ta, placeholder);
  lv_obj_set_width(ta, w);
  lv_obj_set_style_bg_color(ta, GC_SURFACE_CONT, 0);
  lv_obj_set_style_text_color(ta, GC_ON_SURFACE, 0);
  lv_obj_set_user_data(ta, (void*)(uintptr_t)(numeric ? 1 : 0));
  lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_DEFOCUSED, NULL);
  return ta;
}

static lv_obj_t* mk_dd(lv_obj_t* parent, const char* opts, lv_coord_t w) {
  lv_obj_t* dd = lv_dropdown_create(parent);
  lv_dropdown_set_options(dd, opts);
  lv_obj_set_width(dd, w);
  lv_obj_set_style_bg_color(dd, GC_SURFACE_CONT, 0);
  lv_obj_set_style_text_color(dd, GC_ON_SURFACE, 0);
  return dd;
}

static lv_obj_t* mk_cb(lv_obj_t* parent, const char* text) {
  lv_obj_t* cb = lv_checkbox_create(parent);
  lv_checkbox_set_text(cb, text);
  lv_obj_set_style_text_color(cb, GC_ON_SURFACE, 0);
  return cb;
}

static lv_obj_t* mk_lbl(lv_obj_t* parent, const char* text) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, GC_ON_SURFACE_VAR, 0);
  return l;
}

// Radio-trio: three checkable buttons, exactly one checked.
static void trio_cb(lv_event_t* e) {
  lv_obj_t** trio = (lv_obj_t**)lv_event_get_user_data(e);
  lv_obj_t* me = lv_event_get_target(e);
  for (int i = 0; i < 3; i++) {
    if (trio[i] == me) lv_obj_add_state(trio[i], LV_STATE_CHECKED);
    else lv_obj_clear_state(trio[i], LV_STATE_CHECKED);
  }
  if (trio == sr_group_btns) {
    for (int i = 0; i < 3; i++) if (trio[i] == me) sr_group = i;
  } else if (trio == seed_mode_btns) {
    for (int i = 0; i < 3; i++) if (trio[i] == me) seed_mode = (i == 0) ? 1 : (i == 1) ? 2 : 8;
    // enable/disable loc fields per mode
    for (int i = 0; i < 8; i++) {
      bool en = (seed_mode == 8) || (seed_mode == 1 && i == 0) ||
                (seed_mode == 2 && (i == 0 || i == 4));
      if (en) lv_obj_clear_state(ta_locs[i], LV_STATE_DISABLED);
      else lv_obj_add_state(ta_locs[i], LV_STATE_DISABLED);
    }
  }
}

static lv_obj_t* mk_trio_btn(lv_obj_t* parent, const char* text,
                             lv_obj_t** trio, int idx, bool checked) {
  lv_obj_t* b = lv_btn_create(parent);
  lv_obj_set_height(b, 30);
  lv_obj_set_style_bg_color(b, GC_SURFACE_CONT, 0);
  lv_obj_set_style_bg_color(b, GC_PRIMARY, LV_STATE_CHECKED);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
  lv_obj_add_event_cb(b, trio_cb, LV_EVENT_CLICKED, trio);
  if (checked) lv_obj_add_state(b, LV_STATE_CHECKED);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, GC_PRIMARY, 0);
  lv_obj_set_style_text_color(l, GC_ON_PRIMARY, LV_STATE_CHECKED);
  lv_obj_center(l);
  trio[idx] = b;
  return b;
}

// ---- Confirm modal -----------------------------------------------------------------
static void request_confirm(const char* title,
                            const char cmds[][TAU_CMD_LEN], uint8_t n,
                            const char* phrase) {
  memcpy(s_pending, cmds, (size_t)n * TAU_CMD_LEN);
  s_pending_n = n;
  snprintf(s_phrase, sizeof(s_phrase), "%s", phrase ? phrase : "");
  lv_label_set_text(s_modal_title, title);
  String preview;
  for (uint8_t i = 0; i < n; i++) {
    if (i) preview += "\n";
    preview += cmds[i];
  }
  lv_label_set_text(s_modal_cmds, preview.c_str());
  if (s_phrase[0]) {
    lv_obj_clear_flag(s_modal_phrase_ta, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(s_modal_phrase_ta, "");
    lv_label_set_text_fmt(s_modal_phrase_lbl, "Type %s to confirm:", s_phrase);
  } else {
    lv_obj_add_flag(s_modal_phrase_ta, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_modal_phrase_lbl, "");
  }
  lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_modal);
  audio_play(SOUND_NAV_ZAP);
}

static void modal_cancel_cb(lv_event_t* e) {
  (void)e;
  lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

static void modal_confirm_cb(lv_event_t* e) {
  (void)e;
  if (s_phrase[0]) {
    const char* typed = lv_textarea_get_text(s_modal_phrase_ta);
    if (strcmp(typed, s_phrase) != 0) {
      set_err("Confirmation phrase does not match.");
      lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
      return;
    }
  }
  if (!tau_send_batch(s_ip, (const char(*)[TAU_CMD_LEN])s_pending, s_pending_n)) {
    set_err("Send failed: queue busy or command too long.");
  } else {
    lv_label_set_text(s_lbl_status, "Queued — sending…");
    lv_obj_set_style_text_color(s_lbl_status, GC_ON_SURFACE_VAR, 0);
  }
  lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

// Convenience: single-command confirm
static void confirm_one(const char* title, const char* cmd, const char* phrase) {
  char cmds[1][TAU_CMD_LEN];
  snprintf(cmds[0], TAU_CMD_LEN, "%s", cmd);
  request_confirm(title, (const char(*)[TAU_CMD_LEN])cmds, 1, phrase);
}

// ---- Command builders (port of TauConfigurator.svelte) -------------------------------
static void dotted_to_spaced(const char* in, char* out, size_t len) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < len; i++)
    out[j++] = (in[i] == '.') ? ' ' : in[i];
  out[j] = '\0';
}

// Sample-rate chips
static void sr_update_preview() {
  String rates;
  uint8_t cnt = 0;
  for (int i = 0; i < 16; i++) {
    if (sr_sel[i]) {
      if (rates.length()) rates += " ";
      rates += String(SAMPLE_RATES[i]);
      cnt++;
    }
  }
  if (!cnt) { lv_label_set_text(sr_preview, "(pick up to 4 rates)"); return; }
  String prev;
  const uint8_t* grp = (sr_group == 0) ? GROUP_A : GROUP_B;
  uint8_t gn = (sr_group == 2) ? 8 : 4;
  for (uint8_t i = 0; i < gn; i++) {
    uint8_t ch = (sr_group == 2) ? (i + 1) : grp[i];
    prev += rates + " " + String(ch) + " ch";
    if (i + 1 < gn) prev += "\n";
  }
  lv_label_set_text(sr_preview, prev.c_str());
}

static void sr_chip_cb(lv_event_t* e) {
  lv_obj_t* chip = lv_event_get_target(e);
  int idx = (int)(uintptr_t)lv_event_get_user_data(e);
  bool on = lv_obj_has_state(chip, LV_STATE_CHECKED);
  if (on) {
    uint8_t cnt = 0;
    for (int i = 0; i < 16; i++) if (sr_sel[i]) cnt++;
    if (cnt >= 4) {   // desktop caps at 4 rates
      lv_obj_clear_state(chip, LV_STATE_CHECKED);
      set_err("Max 4 sample rates per channel.");
      return;
    }
  }
  sr_sel[idx] = on;
  sr_update_preview();
}

static void sr_clear_cb(lv_event_t* e) {
  (void)e;
  memset(sr_sel, 0, sizeof(sr_sel));
  for (int i = 0; i < 16; i++) lv_obj_clear_state(sr_chips[i], LV_STATE_CHECKED);
  sr_update_preview();
}

static void sr_apply_cb(lv_event_t* e) {
  (void)e;
  char rates[32] = "";
  uint8_t cnt = 0;
  for (int i = 0; i < 16; i++) {
    if (sr_sel[i]) {
      if (rates[0]) strlcat(rates, " ", sizeof(rates));
      char tmp[8];
      snprintf(tmp, sizeof(tmp), "%u", SAMPLE_RATES[i]);
      strlcat(rates, tmp, sizeof(rates));
      cnt++;
    }
  }
  if (!cnt) { set_err("Pick at least one sample rate."); return; }
  char cmds[8][TAU_CMD_LEN];
  const uint8_t* grp = (sr_group == 0) ? GROUP_A : GROUP_B;
  uint8_t gn = (sr_group == 2) ? 8 : 4;
  for (uint8_t i = 0; i < gn; i++) {
    uint8_t ch = (sr_group == 2) ? (i + 1) : grp[i];
    snprintf(cmds[i], TAU_CMD_LEN, "%s %u ch", rates, ch);
  }
  request_confirm("Apply Sample Rate", (const char(*)[TAU_CMD_LEN])cmds, gn, NULL);
}

static void pga_apply_cb(lv_event_t* e) {
  (void)e;
  static const uint16_t gains[5] = {1, 2, 4, 8, 12};
  uint16_t gain = gains[lv_dropdown_get_selected(dd_pga_gain)];
  uint16_t sel = lv_dropdown_get_selected(dd_pga_chan);   // 0-7 ch, 8=A, 9=B, 10=ALL
  char cmds[8][TAU_CMD_LEN];
  uint8_t n = 0;
  if (sel < 8) {
    snprintf(cmds[n++], TAU_CMD_LEN, "%u %u AMP", gain, sel + 1);
  } else {
    const uint8_t* grp = (sel == 8) ? GROUP_A : GROUP_B;
    uint8_t gn = (sel == 10) ? 8 : 4;
    for (uint8_t i = 0; i < gn; i++) {
      uint8_t ch = (sel == 10) ? (i + 1) : grp[i];
      snprintf(cmds[n++], TAU_CMD_LEN, "%u %u AMP", gain, ch);
    }
  }
  request_confirm("Apply PGA Gain", (const char(*)[TAU_CMD_LEN])cmds, n, NULL);
}

// Network
static void net_apply_cb(lv_event_t* e) {
  (void)e;
  char cmds[6][TAU_CMD_LEN];
  uint8_t n = 0;
  const char* port = lv_textarea_get_text(ta_port);
  if (port[0]) {
    long p = atol(port);
    if (p < 1 || p > 65535) { set_err("Port (Data) must be 1-65535."); return; }
    snprintf(cmds[n++], TAU_CMD_LEN, "0 %ld port", p);
  }
  struct { lv_obj_t* ta; const char* key; } fields[4] = {
    {ta_gw, "gateway"}, {ta_mask, "mask"}, {ta_dns, "dns"}, {ta_ntp, "ntp"},
  };
  for (int i = 0; i < 4; i++) {
    const char* v = lv_textarea_get_text(fields[i].ta);
    if (v[0]) {
      char spaced[40];
      dotted_to_spaced(v, spaced, sizeof(spaced));
      snprintf(cmds[n++], TAU_CMD_LEN, "%s %s", spaced, fields[i].key);
    }
  }
  uint16_t ll = lv_dropdown_get_selected(dd_llsps);
  if (ll > 0) snprintf(cmds[n++], TAU_CMD_LEN, "\"%u\" LLSPS", AUX_SPS[ll - 1]);
  if (!n) { set_err("Nothing to send — fill at least one field."); return; }
  request_confirm("Apply Network Settings", (const char(*)[TAU_CMD_LEN])cmds, n, NULL);
}

// Protocol
static void proto_cb(lv_event_t* e) {
  const char* p = (const char*)lv_event_get_user_data(e);
  char title[32];
  snprintf(title, sizeof(title), "Set Protocol: %s", p);
  confirm_one(title, p, NULL);
}

static bool valid_alnum(const char* v, uint8_t min, uint8_t max, bool space_ok) {
  size_t n = strlen(v);
  if (n < min || n > max) return false;
  for (size_t i = 0; i < n; i++) {
    char c = v[i];
    if (!isalnum((unsigned char)c) && !(space_ok && c == ' ')) return false;
  }
  return true;
}

static void seedlocs_cb(lv_event_t* e) {
  (void)e;
  char cmds[8][TAU_CMD_LEN];
  uint8_t n = 0;
  if (seed_mode == 1) {
    const char* v = lv_textarea_get_text(ta_locs[0]);
    if (!valid_alnum(v, 2, 2, true)) { set_err("Chan 1 must be exactly 2 chars."); return; }
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" set-seedloc", v);
  } else if (seed_mode == 2) {
    const uint8_t idx[2] = {0, 4};
    for (uint8_t s = 0; s < 2; s++) {
      const char* v = lv_textarea_get_text(ta_locs[idx[s]]);
      if (!valid_alnum(v, 2, 2, true)) { set_err("Enabled loc fields need 2 chars."); return; }
      snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" %u 2 set-seedloc", v, s + 1);
    }
  } else {
    for (uint8_t i = 0; i < 8; i++) {
      const char* v = lv_textarea_get_text(ta_locs[i]);
      if (!valid_alnum(v, 2, 2, true)) { set_err("All 8 loc fields need 2 chars."); return; }
      snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" %u 8 set-seedloc", v, i + 1);
    }
  }
  request_confirm("Submit Location IDs", (const char(*)[TAU_CMD_LEN])cmds, n, NULL);
}

static void seedstreams_cb(lv_event_t* e) {
  (void)e;
  char cmds[8][TAU_CMD_LEN];
  uint8_t n = 0;
  for (uint8_t i = 0; i < 8; i++) {
    const char* v = lv_textarea_get_text(ta_streams[i]);
    if (!v[0]) continue;
    if (!valid_alnum(v, 2, 2, false)) { set_err("Stream IDs: 2 alphanumeric chars."); return; }
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" %u set-schar", v, i + 1);
  }
  if (!n) { set_err("Enter at least one stream ID."); return; }
  request_confirm("Submit Stream IDs", (const char(*)[TAU_CMD_LEN])cmds, n, NULL);
}

static void seedsta_cb(lv_event_t* e) {
  (void)e;
  const char* v = lv_textarea_get_text(ta_seedsta);
  if (!valid_alnum(v, 1, 4, false)) { set_err("Station ID: max 4 alphanumeric chars."); return; }
  char cmd[TAU_CMD_LEN];
  snprintf(cmd, sizeof(cmd), "\"%s\" set-seedsta", v);
  confirm_one("Set SEED Station ID", cmd, NULL);
}

static void seednet_cb(lv_event_t* e) {
  (void)e;
  const char* v = lv_textarea_get_text(ta_seednet);
  if (!valid_alnum(v, 1, 2, false)) { set_err("Network ID: max 2 alphanumeric chars."); return; }
  char cmd[TAU_CMD_LEN];
  snprintf(cmd, sizeof(cmd), "\"%s\" set-seednet", v);
  confirm_one("Set SEED Network ID", cmd, NULL);
}

// Aux
static void aux_apply_cb(lv_event_t* e) {
  (void)e;
  static const char* tcpflush_vals[6] = {"0", "1", "2", "3", "82", "83"};
  static const char* emode_vals[3] = {"0", "10", "100"};
  char cmds[8][TAU_CMD_LEN];
  uint8_t n = 0;
  uint16_t s;
  s = lv_dropdown_get_selected(dd_maxtx);
  if (s > 0) snprintf(cmds[n++], TAU_CMD_LEN, "\"%u\" maxtx", AUX_SPS[s - 1]);
  s = lv_dropdown_get_selected(dd_aux_llsps);
  if (s > 0) snprintf(cmds[n++], TAU_CMD_LEN, "\"%u\" LLSPS", AUX_SPS[s - 1]);
  s = lv_dropdown_get_selected(dd_tcpflush);
  if (s > 0) snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" TCPFLUSH", tcpflush_vals[s - 1]);
  s = lv_dropdown_get_selected(dd_emode);
  if (s > 0) snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" EMODE", emode_vals[s - 1]);
  const char* lr = lv_textarea_get_text(ta_lograte);
  if (lr[0]) {
    long v = atol(lr);
    if (v < 1 || v > 60) { set_err("LOGRATE must be 1-60."); return; }
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%ld\" lograte", v);
  }
  const char* lrh = lv_textarea_get_text(ta_lograteh);
  if (lrh[0]) {
    long v = atol(lrh);
    if (v < 1 || v > 60) { set_err("LOGRATEH must be 1-60."); return; }
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%ld\" lograteh", v);
  }
  char sm[12] = "^";
  for (int i = 0; i < 8; i++)
    strlcat(sm, lv_obj_has_state(cb_sm[i], LV_STATE_CHECKED) ? "y" : "n", sizeof(sm));
  if (strcmp(sm, "^nnnnnnnn") != 0)
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" statusmode", sm);
  char smh[12] = "^";
  for (int i = 0; i < 8; i++)
    strlcat(smh, lv_obj_has_state(cb_smh[i], LV_STATE_CHECKED) ? "y" : "n", sizeof(smh));
  if (strcmp(smh, "^nnnnnnnn") != 0)
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" statusmodeh", smh);
  if (!n) { set_err("Nothing to submit — touch at least one field."); return; }
  request_confirm("Apply Aux Configuration", (const char(*)[TAU_CMD_LEN])cmds, n, NULL);
}

// Trigger
// Single value -> 8 copies when all checked, else pass tokens through.
static bool chan_value(lv_obj_t* ta, lv_obj_t* all_cb, char* out, size_t len) {
  const char* raw = lv_textarea_get_text(ta);
  while (*raw == ' ') raw++;
  if (!*raw) return false;
  if (lv_obj_has_state(all_cb, LV_STATE_CHECKED)) {
    char first[16];
    size_t i = 0;
    while (raw[i] && raw[i] != ' ' && i < sizeof(first) - 1) { first[i] = raw[i]; i++; }
    first[i] = '\0';
    out[0] = '\0';
    for (int k = 0; k < 8; k++) {
      if (k) strlcat(out, " ", len);
      strlcat(out, first, len);
    }
  } else {
    snprintf(out, len, "%s", raw);
  }
  return true;
}

static void trig_apply_cb(lv_event_t* e) {
  (void)e;
  char cmds[12][TAU_CMD_LEN];
  uint8_t n = 0;
  const char* pre = lv_textarea_get_text(ta_pre);
  if (pre[0]) {
    long v = atol(pre);
    if (v < 0 || v > 255) { set_err("Pre-trigger must be 0-255."); return; }
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%ld\" pre-trig", v);
  }
  const char* post = lv_textarea_get_text(ta_post);
  if (post[0]) {
    long v = atol(post);
    if (v < 0 || v > 255) { set_err("Post-trigger must be 0-255."); return; }
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%ld\" post-trig", v);
  }
  char hm[12] = "#";
  for (int i = 0; i < 8; i++)
    strlcat(hm, lv_obj_has_state(cb_hp[i], LV_STATE_CHECKED) ? "y" : "n", sizeof(hm));
  if (strcmp(hm, "#nnnnnnnn") != 0)
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" HPMASK", hm);
  uint16_t s = lv_dropdown_get_selected(dd_hpsps);
  if (s > 0) snprintf(cmds[n++], TAU_CMD_LEN, "\"%u\" hpsps", AUX_SPS[s - 1]);
  s = lv_dropdown_get_selected(dd_hptype);
  if (s > 0) snprintf(cmds[n++], TAU_CMD_LEN, "\"%u\" HPTYPE", s - 1);
  char vals[80];
  if (chan_value(ta_lvl, cb_lvl_all, vals, sizeof(vals)))
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" microg", vals);
  char bm[12] = "#";
  for (int i = 0; i < 8; i++)
    strlcat(bm, lv_obj_has_state(cb_bp[i], LV_STATE_CHECKED) ? "y" : "n", sizeof(bm));
  if (strcmp(bm, "#nnnnnnnn") != 0)
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" BPMASK", bm);
  s = lv_dropdown_get_selected(dd_bpsps);
  if (s > 0) snprintf(cmds[n++], TAU_CMD_LEN, "\"%u\" bpsps", AUX_SPS[s - 1]);
  s = lv_dropdown_get_selected(dd_bptype);
  if (s > 0) {
    static const uint8_t bp_vals[4] = {0, 1, 2, 5};
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%u\" bptype", bp_vals[s - 1]);
  }
  if (chan_value(ta_sta, cb_sta_all, vals, sizeof(vals)))
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" STA", vals);
  if (chan_value(ta_lta, cb_lta_all, vals, sizeof(vals)))
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" LTA", vals);
  if (chan_value(ta_rat, cb_rat_all, vals, sizeof(vals)))
    snprintf(cmds[n++], TAU_CMD_LEN, "\"%s\" ratios", vals);
  if (!n) { set_err("Nothing to submit — fill at least one field."); return; }
  request_confirm("Apply Trigger Configuration", (const char(*)[TAU_CMD_LEN])cmds, n, NULL);
}

static void trig_preset_cb(lv_event_t* e) {
  uintptr_t kind = (uintptr_t)lv_event_get_user_data(e);
  if (kind == 0) confirm_one("Enable Triggering", "1 Trigger", NULL);
  else if (kind == 1) confirm_one("Disable Triggering — no events will be logged",
                                  "0 Trigger", NULL);
  else confirm_one("Manual Trigger", "trig", NULL);
}

// Storage / GPS
static void storage_cb(lv_event_t* e) {
  bool en = (bool)(uintptr_t)lv_event_get_user_data(e);
  confirm_one(en ? "Enable SD Storage" : "Disable SD Storage",
              en ? "0 DISSD" : "87 DISSD", NULL);
}
static void gps_cb(lv_event_t* e) {
  bool en = (bool)(uintptr_t)lv_event_get_user_data(e);
  confirm_one(en ? "Enable GPS Port" : "Disable GPS Port",
              en ? "0 GPSX" : "1 GPSX", NULL);
}

// Rotation
static void setrot_cb(lv_event_t* e) {
  (void)e;
  const char* v = lv_textarea_get_text(ta_rot);
  if (!v[0]) { set_err("Enter an angle, e.g. 45 or -12.5."); return; }
  char cmd[TAU_CMD_LEN];
  snprintf(cmd, sizeof(cmd), "%s SETROT", v);
  confirm_one("Set Horizontal Rotation", cmd, NULL);
}
static void rotm_cb(lv_event_t* e) {
  (void)e;
  const char* v = lv_textarea_get_text(ta_rotm);
  if (!v[0]) { set_err("Enter a raw ROTM command."); return; }
  if (strlen(v) >= TAU_CMD_LEN) { set_err("ROTM command too long."); return; }
  confirm_one("Set Rotation Matrix (ROTM)", v, NULL);
}

// Danger zone
static void reboot_cb(lv_event_t* e) {
  (void)e;
  confirm_one("Re-Boot Tau", "reboot", "REBOOT");
}
static void factory_cb(lv_event_t* e) {
  (void)e;
  char cmds[24][TAU_CMD_LEN];
  for (uint8_t i = 0; i < 24; i++)
    snprintf(cmds[i], TAU_CMD_LEN, "%s", FACTORY_RESET_CMDS[i]);
  request_confirm("FACTORY RESET", (const char(*)[TAU_CMD_LEN])cmds, 24,
                  "FACTORY RESET");
}
static void savenvm_cb(lv_event_t* e) {
  (void)e;
  confirm_one("Save configuration to NVM", "seed", NULL);
}

// Overview
static void read_cfg_cb(lv_event_t* e) {
  (void)e;
  if (!tau_fetch(s_ip)) set_err("Fetch failed: queue busy.");
}

// Back
static void back_cb(lv_event_t* e);

// ---- Poll timer -----------------------------------------------------------------------
static void tau_poll(lv_timer_t* t) {
  (void)t;
  if (!s_scr || lv_scr_act() != s_scr) return;

  if (tau_busy()) {
    lv_label_set_text_fmt(s_lbl_status, "%s", tau_status());
    lv_obj_set_style_text_color(s_lbl_status, GC_ON_SURFACE_VAR, 0);
  }

  if (tau_rows_generation() != s_ov_seen_gen) {
    s_ov_seen_gen = tau_rows_generation();
    lv_obj_clean(s_ov_col);
    static char rows[TAU_ROWS_MAX][TAU_ROW_LEN];
    uint8_t n = tau_rows_snapshot(rows, TAU_ROWS_MAX);
    if (!n) mk_lbl(s_ov_col, "(no rows)");
    for (uint8_t i = 0; i < n; i++) {
      lv_obj_t* l = lv_label_create(s_ov_col);
      if (strncmp(rows[i], "## ", 3) == 0) {
        lv_label_set_text(l, rows[i] + 3);
        lv_obj_set_style_text_color(l, GC_PRIMARY, 0);
      } else {
        lv_label_set_text(l, rows[i]);
        lv_obj_set_style_text_color(l, GC_ON_SURFACE, 0);
      }
      lv_label_set_long_mode(l, LV_LABEL_LONG_SCROLL_CIRCULAR);
      lv_obj_set_width(l, LV_PCT(100));
    }
  }

  if (tau_log_generation() != s_res_seen_gen) {
    s_res_seen_gen = tau_log_generation();
    lv_obj_clean(s_res_col);
    static char lines[TAU_LOG_LINES][TAU_LOG_LEN];
    uint8_t n = tau_log_snapshot(lines, TAU_LOG_LINES);
    if (!n) mk_lbl(s_res_col, "(no commands sent yet)");
    for (uint8_t i = 0; i < n; i++) {
      lv_obj_t* l = lv_label_create(s_res_col);
      lv_label_set_text(l, lines[i]);
      bool ok = strncmp(lines[i], "OK ", 3) == 0;
      bool err = strncmp(lines[i], "ERR", 3) == 0;
      lv_obj_set_style_text_color(l, ok ? GC_PRIMARY : (err ? GC_DANGER : GC_ON_SURFACE_VAR), 0);
      lv_label_set_long_mode(l, LV_LABEL_LONG_SCROLL_CIRCULAR);
      lv_obj_set_width(l, LV_PCT(100));
    }
  }
}

// ---- Mask grid helper (8 checkboxes, flex-wrapped) -------------------------------------
static void mk_mask8(lv_obj_t* card, const char* title, lv_obj_t* cbs[8],
                     const char* const labels[8]) {
  mk_lbl(card, title);
  lv_obj_t* grid = lv_obj_create(card);
  lv_obj_set_width(grid, LV_PCT(100));
  lv_obj_set_height(grid, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 0, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(grid, 2, 0);
  lv_obj_set_style_pad_column(grid, 4, 0);
  lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  for (int i = 0; i < 8; i++) cbs[i] = mk_cb(grid, labels[i]);
}

static const char* const CH_LABELS[8] = {"1","2","3","4","5","6","7","8"};

// ---- Screen build -------------------------------------------------------------------------
static void build_screen() {
  s_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0F172A), 0);

  // Header
  lv_obj_t* back = lv_btn_create(s_scr);
  lv_obj_set_size(back, 64, 30);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
  lv_obj_set_style_bg_color(back, GC_SURFACE_CONT, 0);
  lv_obj_set_style_radius(back, 8, 0);
  lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bl = lv_label_create(back);
  lv_label_set_text(bl, "< LAN");
  lv_obj_set_style_text_color(bl, GC_PRIMARY, 0);
  lv_obj_center(bl);

  s_lbl_ip = lv_label_create(s_scr);
  lv_label_set_text(s_lbl_ip, "TAU");
  lv_obj_set_style_text_color(s_lbl_ip, GC_PRIMARY, 0);
  lv_obj_align(s_lbl_ip, LV_ALIGN_TOP_RIGHT, -6, 10);

  s_lbl_status = lv_label_create(s_scr);
  lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_lbl_status, 308);
  lv_label_set_text(s_lbl_status, "");
  lv_obj_set_style_text_color(s_lbl_status, GC_ON_SURFACE_VAR, 0);
  lv_obj_align(s_lbl_status, LV_ALIGN_TOP_LEFT, 6, 38);

  // Scrollable sections column
  s_col = lv_obj_create(s_scr);
  lv_obj_set_size(s_col, 320, 480 - 56);
  lv_obj_align(s_col, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_set_style_bg_opa(s_col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_col, 0, 0);
  lv_obj_set_style_pad_all(s_col, 6, 0);
  lv_obj_set_style_pad_row(s_col, 8, 0);
  lv_obj_set_flex_flow(s_col, LV_FLEX_FLOW_COLUMN);

  // ── Overview ──────────────────────────────────────────────────────────────
  {
    lv_obj_t* c = mk_card(s_col, "Overview", false);
    lv_obj_t* r = mk_row(c);
    mk_btn(r, "READ CONFIG", read_cfg_cb, false);
    s_ov_col = lv_obj_create(c);
    lv_obj_set_width(s_ov_col, LV_PCT(100));
    lv_obj_set_height(s_ov_col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_ov_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ov_col, 0, 0);
    lv_obj_set_style_pad_all(s_ov_col, 0, 0);
    lv_obj_set_flex_flow(s_ov_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_ov_col, LV_OBJ_FLAG_SCROLLABLE);
    mk_lbl(s_ov_col, "No configuration loaded.");
  }

  // ── Sample Rate + PGA ─────────────────────────────────────────────────────
  {
    lv_obj_t* c = mk_card(s_col, "Sample Rate", false);
    lv_obj_t* r = mk_row(c);
    mk_trio_btn(r, "Sensor A", sr_group_btns, 0, true);
    mk_trio_btn(r, "Sensor B", sr_group_btns, 1, false);
    mk_trio_btn(r, "All", sr_group_btns, 2, false);

    lv_obj_t* chips = lv_obj_create(c);
    lv_obj_set_width(chips, LV_PCT(100));
    lv_obj_set_height(chips, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chips, 0, 0);
    lv_obj_set_style_pad_all(chips, 0, 0);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(chips, 4, 0);
    lv_obj_set_style_pad_column(chips, 4, 0);
    lv_obj_clear_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 16; i++) {
      lv_obj_t* b = lv_btn_create(chips);
      lv_obj_set_size(b, 60, 28);
      lv_obj_set_style_bg_color(b, GC_SURFACE_CONT, 0);
      lv_obj_set_style_bg_color(b, GC_PRIMARY, LV_STATE_CHECKED);
      lv_obj_set_style_radius(b, 6, 0);
      lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
      lv_obj_add_event_cb(b, sr_chip_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
      lv_obj_t* l = lv_label_create(b);
      lv_label_set_text_fmt(l, "%u", SAMPLE_RATES[i]);
      lv_obj_set_style_text_color(l, GC_PRIMARY, 0);
      lv_obj_set_style_text_color(l, GC_ON_PRIMARY, LV_STATE_CHECKED);
      lv_obj_center(l);
      sr_chips[i] = b;
    }
    sr_preview = mk_lbl(c, "(pick up to 4 rates)");
    lv_obj_t* r2 = mk_row(c);
    mk_btn(r2, "Clear", sr_clear_cb, false);
    mk_btn(r2, "Apply Sample Rate", sr_apply_cb, false);

    mk_lbl(c, "Amplifier (PGA)");
    lv_obj_t* r3 = mk_row(c);
    dd_pga_chan = mk_dd(r3, "Ch 1\nCh 2\nCh 3\nCh 4\nCh 5\nCh 6\nCh 7\nCh 8\nSensor A (1-4)\nSensor B (5-8)\nAll", 130);
    dd_pga_gain = mk_dd(r3, "1\n2\n4\n8\n12", 70);
    mk_btn(r3, "Apply PGA", pga_apply_cb, false);
  }

  // ── Network ───────────────────────────────────────────────────────────────
  {
    lv_obj_t* c = mk_card(s_col, "Network", false);
    lv_obj_t* r;
    r = mk_row(c); mk_lbl(r, "Port (Data):"); ta_port = mk_ta(r, "18000", 5, true, 110);
    r = mk_row(c); mk_lbl(r, "Gateway:");     ta_gw   = mk_ta(r, "192.168.1.1", 15, false, 150);
    r = mk_row(c); mk_lbl(r, "Subnet Mask:"); ta_mask = mk_ta(r, "", 15, false, 150);
    lv_textarea_set_text(ta_mask, "255.255.255.0");
    r = mk_row(c); mk_lbl(r, "DNS:");         ta_dns  = mk_ta(r, "", 15, false, 150);
    r = mk_row(c); mk_lbl(r, "NTP:");         ta_ntp  = mk_ta(r, "", 15, false, 150);
    r = mk_row(c); mk_lbl(r, "UDP LL:");
    String opts = "-- No Change --";
    for (int i = 0; i < 13; i++) opts += "\n" + String(AUX_SPS[i]);
    dd_llsps = mk_dd(r, opts.c_str(), 150);
    r = mk_row(c); mk_btn(r, "Apply Network Settings", net_apply_cb, false);
  }

  // ── Protocol ──────────────────────────────────────────────────────────────
  {
    lv_obj_t* c = mk_card(s_col, "Protocol", false);
    lv_obj_t* r = mk_row(c);
    lv_obj_t* pb;
    pb = mk_btn(r, "GAIA", NULL, false);
    lv_obj_add_event_cb(pb, proto_cb, LV_EVENT_CLICKED, (void*)"GAIA");
    pb = mk_btn(r, "SEED", NULL, false);
    lv_obj_add_event_cb(pb, proto_cb, LV_EVENT_CLICKED, (void*)"SEED");
    pb = mk_btn(r, "GCF", NULL, false);
    lv_obj_add_event_cb(pb, proto_cb, LV_EVENT_CLICKED, (void*)"GCF");

    mk_lbl(c, "SEED Location ID Setup");
    lv_obj_t* rm = mk_row(c);
    mk_trio_btn(rm, "One", seed_mode_btns, 0, true);
    mk_trio_btn(rm, "Two", seed_mode_btns, 1, false);
    mk_trio_btn(rm, "Eight", seed_mode_btns, 2, false);
    lv_obj_t* lg = lv_obj_create(c);
    lv_obj_set_width(lg, LV_PCT(100));
    lv_obj_set_height(lg, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(lg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lg, 0, 0);
    lv_obj_set_style_pad_all(lg, 0, 0);
    lv_obj_set_flex_flow(lg, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(lg, 4, 0);
    lv_obj_set_style_pad_column(lg, 4, 0);
    lv_obj_clear_flag(lg, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 8; i++) {
      ta_locs[i] = mk_ta(lg, "loc", 2, false, 66);
      if (i != 0) lv_obj_add_state(ta_locs[i], LV_STATE_DISABLED);
    }
    lv_obj_t* rl = mk_row(c);
    mk_btn(rl, "Submit Location IDs", seedlocs_cb, false);

    mk_lbl(c, "SEED Stream IDs (e.g. HZ, HE, HN)");
    lv_obj_t* sg = lv_obj_create(c);
    lv_obj_set_width(sg, LV_PCT(100));
    lv_obj_set_height(sg, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(sg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sg, 0, 0);
    lv_obj_set_style_pad_all(sg, 0, 0);
    lv_obj_set_flex_flow(sg, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(sg, 4, 0);
    lv_obj_set_style_pad_column(sg, 4, 0);
    lv_obj_clear_flag(sg, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 8; i++) ta_streams[i] = mk_ta(sg, "XX", 2, false, 66);
    lv_obj_t* rs = mk_row(c);
    mk_btn(rs, "Submit Stream IDs", seedstreams_cb, false);

    lv_obj_t* r1 = mk_row(c);
    mk_lbl(r1, "Station:");
    ta_seedsta = mk_ta(r1, "TAU1", 4, false, 90);
    mk_btn(r1, "Set", seedsta_cb, false);
    lv_obj_t* r2 = mk_row(c);
    mk_lbl(r2, "Network:");
    ta_seednet = mk_ta(r2, "GC", 2, false, 60);
    mk_btn(r2, "Set", seednet_cb, false);
  }

  // ── Aux ───────────────────────────────────────────────────────────────────
  {
    lv_obj_t* c = mk_card(s_col, "Aux", false);
    String sps_opts = "-- No Change --";
    for (int i = 0; i < 13; i++) sps_opts += "\n" + String(AUX_SPS[i]);
    lv_obj_t* r = mk_row(c);
    mk_lbl(r, "MAXTX:"); dd_maxtx = mk_dd(r, sps_opts.c_str(), 130);
    r = mk_row(c);
    mk_lbl(r, "LLSPS:"); dd_aux_llsps = mk_dd(r, sps_opts.c_str(), 130);
    r = mk_row(c);
    mk_lbl(r, "TCPFLUSH:");
    dd_tcpflush = mk_dd(r, "-- No Change --\n0 (nagle)\n1 (no nagle)\n2 (nagle fifo)\n3 (no nagle fifo)\n82 (NAGLE FIFO..)\n83 (no NAGLE FIFO..)", 160);
    r = mk_row(c);
    mk_lbl(r, "EMODE:");
    dd_emode = mk_dd(r, "-- No Change --\nAuto-negotiate\n10 Mbit (Forced)\n100 Mbit (Forced)", 170);
    r = mk_row(c);
    mk_lbl(r, "LOGRATE:");   ta_lograte  = mk_ta(r, "1-60", 2, true, 60);
    mk_lbl(r, "LOGRATEH:");  ta_lograteh = mk_ta(r, "1-60", 2, true, 60);
    mk_mask8(c, "STATUSMODE (normal-rate)", cb_sm, AUX_LABELS);
    mk_mask8(c, "STATUSMODEH (high-rate)", cb_smh, AUX_LABELS);
    r = mk_row(c);
    mk_lbl(r, "Only touched fields are sent.");
    mk_btn(r, "Apply Aux Config", aux_apply_cb, false);
  }

  // ── Trigger ───────────────────────────────────────────────────────────────
  {
    lv_obj_t* c = mk_card(s_col, "Trigger", false);
    lv_obj_t* r = mk_row(c);
    mk_lbl(r, "Pre (0-255):");  ta_pre  = mk_ta(r, "0", 3, true, 60);
    mk_lbl(r, "Post:");         ta_post = mk_ta(r, "0", 3, true, 60);
    mk_mask8(c, "High-Pass / Level — channel mask", cb_hp, CH_LABELS);
    r = mk_row(c);
    String sps_opts = "-- No Change --";
    for (int i = 0; i < 13; i++) sps_opts += "\n" + String(AUX_SPS[i]);
    mk_lbl(r, "HP sps:"); dd_hpsps = mk_dd(r, sps_opts.c_str(), 120);
    r = mk_row(c);
    mk_lbl(r, "HP type:");
    dd_hptype = mk_dd(r, "-- No Change --\n0 (Disable)\n1 (100 sec)\n2 (200 sec)\n3 (1000 sec)", 150);
    r = mk_row(c);
    mk_lbl(r, "Level (ADU):"); ta_lvl = mk_ta(r, "1000", 40, false, 100);
    cb_lvl_all = mk_cb(r, "all");
    mk_mask8(c, "Band-Pass / STA-LTA — channel mask", cb_bp, CH_LABELS);
    r = mk_row(c);
    mk_lbl(r, "BP sps:"); dd_bpsps = mk_dd(r, sps_opts.c_str(), 120);
    r = mk_row(c);
    mk_lbl(r, "BP type:");
    dd_bptype = mk_dd(r, "-- No Change --\n0 (Off)\n1 (50-450Hz)\n2 (100-450Hz)\n5 (250-450Hz)", 160);
    r = mk_row(c);
    mk_lbl(r, "STA:"); ta_sta = mk_ta(r, "single or 8", 40, false, 110);
    cb_sta_all = mk_cb(r, "all");
    r = mk_row(c);
    mk_lbl(r, "LTA:"); ta_lta = mk_ta(r, "single or 8", 40, false, 110);
    cb_lta_all = mk_cb(r, "all");
    r = mk_row(c);
    mk_lbl(r, "Ratios:"); ta_rat = mk_ta(r, "single or 8", 40, false, 110);
    cb_rat_all = mk_cb(r, "all");
    r = mk_row(c);
    lv_obj_t* b;
    b = mk_btn(r, "Enable Trig", NULL, false);
    lv_obj_add_event_cb(b, trig_preset_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)0);
    b = mk_btn(r, "Disable Trig", NULL, true);
    lv_obj_add_event_cb(b, trig_preset_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)1);
    b = mk_btn(r, "Manual Trig", NULL, false);
    lv_obj_add_event_cb(b, trig_preset_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)2);
    r = mk_row(c);
    mk_lbl(r, "Only filled fields are sent.");
    mk_btn(r, "Apply Trigger Params", trig_apply_cb, false);
  }

  // ── Storage & GPS ─────────────────────────────────────────────────────────
  {
    lv_obj_t* c = mk_card(s_col, "Storage & GPS", false);
    mk_lbl(c, "SD Storage (DISSD)");
    lv_obj_t* r = mk_row(c);
    lv_obj_t* b;
    b = mk_btn(r, "Enable", NULL, false);
    lv_obj_add_event_cb(b, storage_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)1);
    b = mk_btn(r, "Disable", NULL, true);
    lv_obj_add_event_cb(b, storage_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)0);
    mk_lbl(c, "GPS Serial Port (GPSX)");
    r = mk_row(c);
    b = mk_btn(r, "Enable", NULL, false);
    lv_obj_add_event_cb(b, gps_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)1);
    b = mk_btn(r, "Disable", NULL, true);
    lv_obj_add_event_cb(b, gps_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)0);
  }

  // ── Rotation ──────────────────────────────────────────────────────────────
  {
    lv_obj_t* c = mk_card(s_col, "Rotation", false);
    mk_lbl(c, "Horizontal Rotation (SETROT)");
    lv_obj_t* r = mk_row(c);
    mk_lbl(r, "Angle:");
    ta_rot = mk_ta(r, "45 or -12.5", 10, false, 110);
    mk_btn(r, "Apply SETROT", setrot_cb, false);
    mk_lbl(c, "Rotation Matrix (ROTM)");
    r = mk_row(c);
    ta_rotm = mk_ta(r, "1 0 0 0 1 0 0 0 1 ROTM", 40, false, 170);
    mk_btn(r, "Apply ROTM", rotm_cb, false);
  }

  // ── Danger Zone ───────────────────────────────────────────────────────────
  {
    lv_obj_t* c = mk_card(s_col, "DANGER ZONE", true);
    lv_obj_t* r = mk_row(c);
    mk_btn(r, "Re-Boot Tau", reboot_cb, true);
    mk_btn(r, "FACTORY RESET", factory_cb, true);
    r = mk_row(c);
    mk_btn(r, "SAVE TO NVM (seed)", savenvm_cb, true);
  }

  // ── Results ───────────────────────────────────────────────────────────────
  {
    lv_obj_t* c = mk_card(s_col, "Results", false);
    s_res_col = lv_obj_create(c);
    lv_obj_set_width(s_res_col, LV_PCT(100));
    lv_obj_set_height(s_res_col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_res_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_res_col, 0, 0);
    lv_obj_set_style_pad_all(s_res_col, 0, 0);
    lv_obj_set_flex_flow(s_res_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_res_col, LV_OBJ_FLAG_SCROLLABLE);
    mk_lbl(s_res_col, "(no commands sent yet)");
  }

  // ── Shared keyboard (hidden until a textarea is focused) ──────────────────
  s_kb = lv_keyboard_create(s_scr);
  lv_obj_set_size(s_kb, 320, 190);
  lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_CANCEL, NULL);

  // ── Confirm modal (top layer) ─────────────────────────────────────────────
  s_modal = lv_obj_create(s_scr);
  lv_obj_set_size(s_modal, 320, 480);
  lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x0F172A), 0);
  lv_obj_set_style_bg_opa(s_modal, LV_OPA_80, 0);
  lv_obj_set_style_border_width(s_modal, 0, 0);
  lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* mcard = lv_obj_create(s_modal);
  lv_obj_set_size(mcard, 300, 360);
  lv_obj_center(mcard);
  lv_obj_set_style_bg_color(mcard, GC_SURFACE, 0);
  lv_obj_set_style_radius(mcard, 10, 0);
  lv_obj_set_style_border_width(mcard, 0, 0);
  lv_obj_set_style_pad_all(mcard, 10, 0);
  lv_obj_set_style_pad_row(mcard, 8, 0);
  lv_obj_set_flex_flow(mcard, LV_FLEX_FLOW_COLUMN);

  s_modal_title = lv_label_create(mcard);
  lv_obj_set_style_text_color(s_modal_title, GC_PRIMARY, 0);
  lv_label_set_text(s_modal_title, "Confirm");

  lv_obj_t* cmds_wrap = lv_obj_create(mcard);
  lv_obj_set_size(cmds_wrap, LV_PCT(100), 150);
  lv_obj_set_style_bg_color(cmds_wrap, GC_SURFACE_CONT, 0);
  lv_obj_set_style_border_width(cmds_wrap, 0, 0);
  lv_obj_set_style_radius(cmds_wrap, 6, 0);
  s_modal_cmds = lv_label_create(cmds_wrap);
  lv_obj_set_style_text_color(s_modal_cmds, GC_ON_SURFACE, 0);
  lv_label_set_text(s_modal_cmds, "");

  s_modal_phrase_lbl = lv_label_create(mcard);
  lv_obj_set_style_text_color(s_modal_phrase_lbl, GC_DANGER, 0);
  lv_label_set_text(s_modal_phrase_lbl, "");
  s_modal_phrase_ta = mk_ta(mcard, "confirmation phrase", 19, false, LV_PCT(100));
  lv_obj_add_flag(s_modal_phrase_ta, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* mrow = mk_row(mcard);
  mk_btn(mrow, "Cancel", modal_cancel_cb, false);
  mk_btn(mrow, "Confirm & Send", modal_confirm_cb, true);

  lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

  s_poll = lv_timer_create(tau_poll, 400, NULL);
}

// Back: declared late because it references s_kb.
static void back_cb(lv_event_t* e) {
  (void)e;
  lv_keyboard_set_textarea(s_kb, NULL);
  lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
  ui_show_main();   // ui.h
}

// ---- Public -------------------------------------------------------------------
void ui_tau_config_open(const char* ip) {
  if (!s_scr) build_screen();
  snprintf(s_ip, sizeof(s_ip), "%s", ip);
  lv_label_set_text_fmt(s_lbl_ip, "TAU  %s", s_ip);
  lv_label_set_text(s_lbl_status, "");
  lv_scr_load(s_scr);
  // Kick off a config read so the overview is populated on entry.
  s_ov_seen_gen = tau_rows_generation();   // wait for the next bump
  if (!tau_fetch(s_ip)) set_err("Fetch failed: queue busy.");
}
