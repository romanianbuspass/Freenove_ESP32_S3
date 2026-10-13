#include "gnss.h"
#include "hal.h"
#include "storage.h"
#include "esp_timer.h"

// Shared, UI-visible state (written only by gnss_task under s_mtx).
static GnssState s_state;
static uint8_t   s_snr[65];
static uint32_t  s_gen = 0;
static SemaphoreHandle_t s_mtx = NULL;

// 1 PPS — ISR-written volatiles (32-bit aligned reads are atomic on Xtensa).
static volatile uint32_t s_pps_count   = 0;
static volatile int64_t  s_pps_last_us = 0;

// Rising-edge ISR, stripped to the thread-safe bare minimum: timestamp +
// counter only. No logging, no queues, no lamp pulses, no UI calls.
// The GNSS tab flashes its 1 PPS lamp by polling gnss_pps_count().
// Glitch gate: edges <900 ms after the last valid one are noise/ringing on
// the divider output — drop them before they can storm the Int WDT.
static void IRAM_ATTR pps_isr() {
  int64_t now = esp_timer_get_time();
  if (s_pps_last_us != 0 && (now - s_pps_last_us) < 900000) return;
  s_pps_last_us = now;
  s_pps_count++;
}

// GSV epoch accumulation state (task-local).
static uint32_t s_last_gsv_ms = 0;
static uint16_t s_cycle_sv = 0;      // SV-in-view summed across constellations
static char     s_cycle_talker = 0;  // constellation letter of current epoch

// ---- Raw NMEA console ring (diagnostic: what is landing on the wire) ---------
static const uint8_t RAW_LINES = 4;
static char     s_raw[RAW_LINES][100];
static uint8_t  s_raw_head = 0;      // next write slot
static uint8_t  s_raw_fill = 0;      // lines stored so far (0..RAW_LINES)
static uint32_t s_raw_gen  = 0;

// Called only from gnss_task, just before handle_sentence().
static void raw_push(const char* line) {
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  snprintf(s_raw[s_raw_head], sizeof(s_raw[0]), "%s", line);
  s_raw_head = (s_raw_head + 1) % RAW_LINES;
  if (s_raw_fill < RAW_LINES) s_raw_fill++;
  s_raw_gen++;
  xSemaphoreGive(s_mtx);
}

uint32_t gnss_raw_generation() { return s_raw_gen; }

// Oldest-to-newest, newline-joined into `out`. Always NUL-terminated.
void gnss_raw_snapshot(char* out, size_t len) {
  if (!out || len == 0) return;
  out[0] = '\0';
  if (!s_mtx) return;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  for (uint8_t i = 0; i < s_raw_fill; i++) {
    uint8_t slot = (s_raw_head + RAW_LINES - s_raw_fill + i) % RAW_LINES;
    size_t cur = strlen(out);
    if (cur + 2 < len)
      snprintf(out + cur, len - cur, "%s%s", cur ? "\n" : "", s_raw[slot]);
  }
  xSemaphoreGive(s_mtx);
}

// ---- NMEA parsing (task-local working copy; committed under mutex) ------------
static bool field(const char* s, uint8_t idx, char* out, size_t outlen) {
  const char* p = s;
  for (uint8_t i = 0; i < idx; i++) {
    p = strchr(p, ',');
    if (!p) return false;
    p++;
  }
  const char* e = strchr(p, ',');
  const char* star = strchr(p, '*');          // checksum starts: "*XX"
  if (star && (!e || star < e)) e = star;     // last field ends at '*'
  size_t n = e ? (size_t)(e - p) : strlen(p);
  if (n >= outlen) n = outlen - 1;
  memcpy(out, p, n);
  out[n] = '\0';
  return true;
}

static double nmea_to_deg(const char* v, const char* hemi) {
  double raw = atof(v);
  int deg = (int)(raw / 100.0);
  double dec = deg + (raw - deg * 100.0) / 60.0;
  if (hemi[0] == 'S' || hemi[0] == 'W') dec = -dec;
  return dec;
}

// Working state owned by gnss_task.
static GnssState w_state;
static uint8_t   w_snr[65];
static bool      w_was_locked = false;

// $GxGGA,time,lat,NS,lon,EW,quality,sats,hdop,alt,M,...
static void parse_gga(const char* body) {
  char lat[16] = "", ns[4] = "", lon[16] = "", ew[4] = "";
  char qual[4] = "", sats[4] = "", alt[16] = "";
  field(body, 1, lat, sizeof(lat));
  field(body, 2, ns, sizeof(ns));
  field(body, 3, lon, sizeof(lon));
  field(body, 4, ew, sizeof(ew));
  field(body, 5, qual, sizeof(qual));
  field(body, 6, sats, sizeof(sats));
  field(body, 8, alt, sizeof(alt));

  w_state.fix_quality = (uint8_t)atoi(qual);
  w_state.sats_used   = (uint8_t)atoi(sats);
  // GSA-only lock requires corroboration (sats in use or valid RMC): the Teseo
  // can emit GSA '3D' at boot with zero SVs — a bare gsa_fix locks falsely.
  w_state.locked      = w_state.fix_quality > 0 ||
                        (w_state.gsa_fix >= 2 && (w_state.sats_used > 0 || w_state.rmc_valid));
  if (w_state.locked && w_state.fix_quality > 0) {
    w_state.lat   = nmea_to_deg(lat, ns);
    w_state.lon   = nmea_to_deg(lon, ew);
    w_state.alt_m = atof(alt);
  }
  w_state.gga_count++;
}

// $GxRMC,time,status,lat,NS,lon,EW,speed,course,date,...*XX
// Status 'A' = data valid (receiver has a fix). Position is only used as a
// fallback when GGA has no fix but GSA/RMC say the receiver is locked.
static void parse_rmc(const char* body) {
  char utc[12] = "", status[4] = "", lat[16] = "", ns[4] = "";
  char lon[16] = "", ew[4] = "", spd[12] = "", crs[12] = "", date[8] = "";
  field(body, 0, utc, sizeof(utc));
  field(body, 1, status, sizeof(status));
  field(body, 2, lat, sizeof(lat));
  field(body, 3, ns, sizeof(ns));
  field(body, 4, lon, sizeof(lon));
  field(body, 5, ew, sizeof(ew));
  field(body, 6, spd, sizeof(spd));
  field(body, 7, crs, sizeof(crs));
  field(body, 8, date, sizeof(date));

  w_state.rmc_valid = (status[0] == 'A');
  snprintf(w_state.utc, sizeof(w_state.utc), "%s", utc);
  snprintf(w_state.date, sizeof(w_state.date), "%s", date);
  w_state.speed_kn  = (float)atof(spd);
  w_state.course_deg = (float)atof(crs);
  if (w_state.rmc_valid && w_state.fix_quality == 0 && lat[0] && lon[0]) {
    w_state.lat = nmea_to_deg(lat, ns);   // fallback: GGA silent, RMC valid
    w_state.lon = nmea_to_deg(lon, ew);
  }
  // GSA-only lock requires corroboration (sats in use or valid RMC): the Teseo
  // can emit GSA '3D' at boot with zero SVs — a bare gsa_fix locks falsely.
  w_state.locked = w_state.fix_quality > 0 ||
                   (w_state.gsa_fix >= 2 && (w_state.sats_used > 0 || w_state.rmc_valid));
  w_state.rmc_count++;
}

// $GxGSA,mode1,fix,sv1..sv12,PDOP,HDOP,VDOP*XX  (one line per constellation)
// Multi-constellation: keep the best fix type across talkers; DOPs follow the
// line that reported the current best fix.
static void parse_gsa(const char* body) {
  char m1[4] = "", fix[4] = "", pd[10] = "", hd[10] = "", vd[10] = "";
  // New epoch if >800 ms since the last GSA line (intra-epoch talker lines
  // arrive back-to-back): reset the max so a fix downgrade can't stick.
  static uint32_t s_last_gsa_ms = 0;
  uint32_t now = millis();
  if (now - s_last_gsa_ms > 800) w_state.gsa_fix = 0;
  s_last_gsa_ms = now;
  field(body, 0, m1, sizeof(m1));
  field(body, 1, fix, sizeof(fix));
  uint8_t f = (uint8_t)atoi(fix);
  if (f >= w_state.gsa_fix) {              // >= : later talker refreshes DOPs
    w_state.gsa_fix = f;
    w_state.gsa_mode = m1[0];
    if (field(body, 14, pd, sizeof(pd))) w_state.pdop = (float)atof(pd);
    if (field(body, 15, hd, sizeof(hd))) w_state.hdop = (float)atof(hd);
    if (field(body, 16, vd, sizeof(vd))) w_state.vdop = (float)atof(vd);
  }
  // GSA-only lock requires corroboration (sats in use or valid RMC): the Teseo
  // can emit GSA '3D' at boot with zero SVs — a bare gsa_fix locks falsely.
  w_state.locked = w_state.fix_quality > 0 ||
                   (w_state.gsa_fix >= 2 && (w_state.sats_used > 0 || w_state.rmc_valid));
  w_state.gsa_count++;
}

// $GxGSV,totalMsgs,msgNum,satsInView, [prn,elev,az,snr] x up to 4
// Multi-constellation: each talker (P=GPS, L=GLONASS, E=Galileo, B=BeiDou)
// sends its own numbered sequence. Sum each constellation's sats_in_view once
// (from its msg 1) and let the task commit the epoch after a GSV silence gap —
// one cycle per epoch, not one per constellation.
static void parse_gsv(const char* body, char talker2) {
  char f[8];
  uint8_t total = 0, msg = 0;
  int sats = -1;
  if (field(body, 0, f, sizeof(f))) total = (uint8_t)atoi(f);
  if (field(body, 1, f, sizeof(f))) msg   = (uint8_t)atoi(f);
  if (field(body, 2, f, sizeof(f))) sats  = atoi(f);
  if (total == 0 || msg == 0) return;

  if (s_cycle_talker == 0) memset(w_snr, 0, sizeof(w_snr));  // fresh epoch
  if (msg == 1 && s_cycle_talker != talker2) {
    s_cycle_talker = talker2;                    // constellation letter
    if (sats > 0) s_cycle_sv += (uint16_t)sats;  // count each talker once
  }
  s_last_gsv_ms = millis();

  for (uint8_t k = 0; k < 4; k++) {
    char prn[8] = "", snr[8] = "";
    if (!field(body, 3 + k * 4, prn, sizeof(prn)) || prn[0] == '\0') break;
    field(body, 3 + k * 4 + 3, snr, sizeof(snr));
    uint8_t p = (uint8_t)atoi(prn);
    if (p >= 1 && p <= 64) w_snr[p] = (uint8_t)atoi(snr);
  }
}

static void commit_state() {
  // Lock-transition logging (kept out of the hot path elsewhere).
  if (w_state.locked != w_was_locked) {
    w_was_locked = w_state.locked;
    char msg[80];
    if (w_state.locked)
      snprintf(msg, sizeof(msg), "LOCK %.5f,%.5f alt %.1f m SV %u",
               w_state.lat, w_state.lon, (double)w_state.alt_m, w_state.sats_used);
    else
      snprintf(msg, sizeof(msg), "lock lost");
    storage_log("GNSS", msg);
  }
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  memcpy(&s_state, &w_state, sizeof(s_state));
  memcpy(s_snr, w_snr, sizeof(s_snr));
  s_gen++;
  xSemaphoreGive(s_mtx);
}

static void handle_sentence(const char* line) {
  if (line[0] != '$' || strlen(line) < 7) return;
  const char* type = line + 3;   // after "$Gx" talker
  const char* body = line + 7;   // past the comma: first field of the body
  bool dirty = false;
  if (strncmp(type, "GGA", 3) == 0)      { parse_gga(body); dirty = true; }
  else if (strncmp(type, "RMC", 3) == 0)  { parse_rmc(body); dirty = true; }
  else if (strncmp(type, "GSA", 3) == 0)  { parse_gsa(body); dirty = true; }
  else if (strncmp(type, "GSV", 3) == 0)  { parse_gsv(body, line[2]); }
  if (dirty) commit_state();
}

// ---- FreeRTOS task: owns UART2 entirely ---------------------------------------
static void gnss_task(void* arg) {
  (void)arg;
  static char line[100];
  uint8_t len = 0;
  for (;;) {
    // A GSV epoch ends when sentences stop arriving: commit the summed
    // multi-constellation totals after 600 ms of GSV silence.
    if (s_cycle_talker && (uint32_t)(millis() - s_last_gsv_ms) > 600) {
      w_state.sats_in_view = s_cycle_sv > 255 ? 255 : (uint8_t)s_cycle_sv;
      w_state.gsv_cycles++;
      s_cycle_sv = 0;
      s_cycle_talker = 0;
      commit_state();
    }
    int avail = Serial2.available();
    if (avail <= 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    hal_activity_pulse(40, 32, 0, 60);   // yellow: NMEA data
    while (avail-- > 0) {
      char c = (char)Serial2.read();
      if (c == '\r' || c == '\n') {
        if (len > 0) { line[len] = '\0'; raw_push(line); handle_sentence(line); len = 0; }
      } else if (len < sizeof(line) - 1) {
        line[len++] = c;
      } else {
        len = 0;   // overrun: drop sentence
      }
    }
  }
}

void gnss_init() {
  // 2 KB RX ring absorbs full multi-constellation GSV bursts
  // (~1.2 KB/epoch at 9600) that overflowed the 256-byte default.
  Serial2.setRxBufferSize(2048);
  Serial2.begin(GNSS_BAUD, SERIAL_8N1, GNSS_RX_PIN, GNSS_TX_PIN);
  Serial.printf("GNSS: UART2 @ %d on RX=%d (listen-only)\n", GNSS_BAUD, GNSS_RX_PIN);

  // 1 PPS input + rising-edge interrupt. INPUT_PULLDOWN holds IO45 low
  // against crosstalk from the adjacent NMEA line while PPS is unwired.
  pinMode(GNSS_PPS_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(GNSS_PPS_PIN), pps_isr, RISING);
  Serial.printf("GNSS: 1 PPS on IO%d (rising edge, IRAM ISR)\n", GNSS_PPS_PIN);

  s_mtx = xSemaphoreCreateMutex();
  memset(&s_state, 0, sizeof(s_state));
  memset(&w_state, 0, sizeof(w_state));
  memset(s_snr, 0, sizeof(s_snr));
  memset(w_snr, 0, sizeof(w_snr));
  memset(s_raw, 0, sizeof(s_raw));

  xTaskCreatePinnedToCore(gnss_task, "gnss", 4096, NULL, 1, NULL, 0);
}

void gnss_snapshot(GnssState* out) {
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  memcpy(out, &s_state, sizeof(s_state));
  xSemaphoreGive(s_mtx);
}

uint8_t gnss_top_sats(GnssSat* out, uint8_t max) {
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  uint8_t n = 0;
  for (uint8_t round = 0; round < max; round++) {
    int best = -1;
    for (uint8_t p = 1; p <= 64; p++) {
      if (s_snr[p] == 0) continue;
      bool taken = false;
      for (uint8_t i = 0; i < n; i++) if (out[i].prn == p) { taken = true; break; }
      if (!taken && (best < 0 || s_snr[p] > s_snr[best])) best = p;
    }
    if (best < 0) break;
    out[n].prn = (uint8_t)best;
    out[n].snr = s_snr[best];
    n++;
  }
  xSemaphoreGive(s_mtx);
  return n;
}

uint32_t gnss_generation() { return s_gen; }
uint32_t gnss_pps_count()  { return s_pps_count; }
int64_t  gnss_pps_last_us(){ return s_pps_last_us; }
