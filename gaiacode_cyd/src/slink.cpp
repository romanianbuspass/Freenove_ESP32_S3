#include "slink.h"
#include "storage.h"
#include <stdarg.h>
#include <WiFi.h>
#include <WiFiClient.h>

// ---- Tunables ------------------------------------------------------------------
static const uint32_t CONN_TIMEOUT_MS  = 4000;
static const uint32_t READ_TIMEOUT_MS  = 15000;  // keepalive SLINFO fills silence
static const uint16_t MSEED_REC = 512;
static const uint16_t SL_FRAME  = 520;
static const uint16_t MAX_DECODE_SAMPLES = 2048;  // per record safety cap

// ---- Shared state ---------------------------------------------------------------
enum SlinkState : uint8_t { SL_IDLE, SL_CONNECTING, SL_STREAMING, SL_ERROR };

static SemaphoreHandle_t s_mtx = NULL;
static TaskHandle_t      s_task = NULL;

static volatile SlinkState s_state = SL_IDLE;
static char     s_error[96] = "";
static char     s_pattern[SLINK_PATTERN_LEN] = "XX.????.??.F??";
static char     s_ip[16] = "";
static uint16_t s_port = 18000;
static volatile bool s_connect_req = false;   // worker picks up params
static volatile bool s_disconnect_req = false;

static uint32_t s_records = 0;
static uint32_t s_matched = 0;
static uint8_t  s_overflow = 0;
static uint32_t s_generation = 0;

struct Lane {
  char     id[SLINK_ID_LEN];
  float    sps;
  uint32_t total;
  uint32_t records;
  int32_t* ring;     // PSRAM: CPU-only buffer, never DMA'd (see slink_init)
  uint32_t w;        // next write index
  uint32_t count;    // <= SLINK_RING
};
static Lane s_lanes[SLINK_MAX_LANES];
static uint8_t s_lane_count = 0;

// ---- miniSEED helpers --------------------------------------------------------------
static uint16_t be_u16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
static int16_t  be_i16(const uint8_t* p) { return (int16_t)be_u16(p); }
static uint32_t be_u32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

struct MseedHead {
  char     net[3];
  char     sta[6];
  char     loc[3];
  char     chan[4];
  uint16_t num_samples;
  float    sps;
  uint16_t data_offset;
  uint16_t first_blockette;
  uint8_t  num_blockettes;
  bool     is_log;
};

static float seed_rate(int16_t factor, int16_t mult) {
  if (factor == 0 || mult == 0) return 0.0f;
  float f = factor, m = mult;
  if (factor > 0) return (mult >= 0) ? f * m : -f / m;
  return (mult >= 0) ? -m / f : 1.0f / (f * m);
}

static void trim_id(char* dst, const uint8_t* src, size_t n) {
  size_t len = n;
  while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\0')) len--;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static bool parse_fsdh(const uint8_t* r, MseedHead& h) {
  trim_id(h.sta, r + 8, 5);
  trim_id(h.loc, r + 13, 2);
  trim_id(h.chan, r + 15, 3);
  trim_id(h.net, r + 18, 2);
  h.num_samples = be_u16(r + 30);
  h.sps = seed_rate(be_i16(r + 32), be_i16(r + 34));
  h.num_blockettes = r[39];
  h.data_offset = be_u16(r + 44);
  h.first_blockette = be_u16(r + 46);
  h.is_log = (memcmp(r + 15, "LOG", 3) == 0);
  return true;
}

// Walk the blockette chain for Blockette 1000; returns encoding (10=Steim1,
// 11=Steim2, 1=i16, 2=i32, 3=f32, 4=f64) and word order, or -1.
static int find_b1000(const uint8_t* r, const MseedHead& h, uint8_t* word_order) {
  uint16_t off = h.first_blockette;
  for (uint8_t seen = 0; off >= 48 && off + 8 <= MSEED_REC && seen < h.num_blockettes; seen++) {
    uint16_t btype = be_u16(r + off);
    if (btype == 1000) {
      *word_order = r[off + 5];
      return r[off + 4];
    }
    uint16_t next = be_u16(r + off + 2);
    if (next == 0 || next == off) break;
    off = next;
  }
  return -1;
}

// ---- Steim decode (port of gaia_dsp/src/steim.rs, libmseed semantics) ---------------
static inline int32_t sign_extend(uint32_t v, uint8_t bits) {
  uint8_t shift = 32 - bits;
  return (int32_t)(v << shift) >> shift;
}

static uint8_t extract_steim1(const uint32_t w[16], uint8_t start, int32_t* diffs, uint8_t max) {
  uint8_t n = 0;
  uint32_t hdr = w[0];
  for (uint8_t i = start; i < 16 && n < max; i++) {
    uint8_t nib = (hdr >> (30 - 2 * i)) & 0x3;
    uint32_t word = w[i];
    switch (nib) {
      case 1:
        for (uint8_t j = 0; j < 4 && n < max; j++)
          diffs[n++] = sign_extend((word >> (8 * (3 - j))) & 0xFF, 8);
        break;
      case 2:
        for (uint8_t j = 0; j < 2 && n < max; j++)
          diffs[n++] = sign_extend((word >> (16 * (1 - j))) & 0xFFFF, 16);
        break;
      case 3:
        diffs[n++] = (int32_t)word;
        break;
      default: break;   // 0 = non-data
    }
  }
  return n;
}

static uint8_t extract_steim2(const uint32_t w[16], uint8_t start, int32_t* diffs, uint8_t max) {
  uint8_t n = 0;
  uint32_t hdr = w[0];
  for (uint8_t i = start; i < 16 && n < max; i++) {
    uint8_t nib = (hdr >> (30 - 2 * i)) & 0x3;
    uint32_t word = w[i];
    if (nib == 1) {          // four 8-bit
      for (uint8_t j = 0; j < 4 && n < max; j++)
        diffs[n++] = sign_extend((word >> (8 * (3 - j))) & 0xFF, 8);
    } else if (nib == 2) {
      uint8_t dnib = (word >> 30) & 0x3;
      if (dnib == 1) {       // one 30-bit
        diffs[n++] = sign_extend(word & 0x3FFFFFFF, 30);
      } else if (dnib == 2) {  // two 15-bit
        for (uint8_t j = 0; j < 2 && n < max; j++)
          diffs[n++] = sign_extend((word >> (15 * (1 - j))) & 0x7FFF, 15);
      } else if (dnib == 3) {  // three 10-bit
        for (uint8_t j = 0; j < 3 && n < max; j++)
          diffs[n++] = sign_extend((word >> (10 * (2 - j))) & 0x3FF, 10);
      }
    } else if (nib == 3) {
      uint8_t dnib = (word >> 30) & 0x3;
      if (dnib == 0) {       // five 6-bit
        for (uint8_t j = 0; j < 5 && n < max; j++)
          diffs[n++] = sign_extend((word >> (6 * (4 - j))) & 0x3F, 6);
      } else if (dnib == 1) {  // six 5-bit
        for (uint8_t j = 0; j < 6 && n < max; j++)
          diffs[n++] = sign_extend((word >> (5 * (5 - j))) & 0x1F, 5);
      } else if (dnib == 2) {  // seven 4-bit
        for (uint8_t j = 0; j < 7 && n < max; j++)
          diffs[n++] = sign_extend((word >> (24 - 4 * j)) & 0xF, 4);
      }
    }
  }
  return n;
}

// Decode a record payload (starting at data_offset) into out[].
// Returns sample count (<= want), or -1 on unsupported encoding.
static int decode_payload(const uint8_t* payload, size_t plen, int encoding,
                          uint16_t want, int32_t* out, uint32_t out_max) {
  if (want > out_max) want = out_max;
  if (want > MAX_DECODE_SAMPLES) want = MAX_DECODE_SAMPLES;
  if (want == 0) return 0;

  switch (encoding) {
    case 1:   // int16
    case 2: { // int32
      uint8_t sz = (encoding == 1) ? 2 : 4;
      uint32_t n = 0;
      for (size_t o = 0; o + sz <= plen && n < want; o += sz)
        out[n++] = (encoding == 1) ? (int32_t)be_i16(payload + o)
                                   : (int32_t)be_u32(payload + o);
      return n;
    }
    case 3:   // float32
    case 4: { // float64
      uint32_t n = 0;
      if (encoding == 3) {
        for (size_t o = 0; o + 4 <= plen && n < want; o += 4) {
          uint32_t u = be_u32(payload + o);
          float f;
          memcpy(&f, &u, 4);
          out[n++] = (int32_t)f;
        }
      } else {
        for (size_t o = 0; o + 8 <= plen && n < want; o += 8) {
          uint64_t u = ((uint64_t)be_u32(payload + o) << 32) | be_u32(payload + o + 4);
          double d;
          memcpy(&d, &u, 8);
          out[n++] = (int32_t)d;
        }
      }
      return n;
    }
    case 10:  // Steim1
    case 11: { // Steim2
      uint32_t have = 0;
      int32_t diffs[105];
      for (size_t f = 0; f + 64 <= plen && have < want; f += 64) {
        uint32_t w[16];
        for (uint8_t i = 0; i < 16; i++) w[i] = be_u32(payload + f + i * 4);
        uint8_t start;
        if (f == 0) {
          out[have++] = (int32_t)w[1];   // W1 = X0 IS sample 0; W2 = Xn (check only)
          if (have >= want) break;
          start = 3;
        } else {
          start = 1;   // continuation: W1..W15 all data, chain from running sample
        }
        uint8_t nd = (encoding == 10)
            ? extract_steim1(w, start, diffs, 105)
            : extract_steim2(w, start, diffs, 105);
        // First diff of the first frame is the encoder continuity diff: skip.
        uint8_t skip = (f == 0 && nd > 0) ? 1 : 0;
        for (uint8_t i = skip; i < nd && have < want; i++)
          out[have] = out[have - 1] + diffs[i], have++;
      }
      return have;
    }
    default:
      return -1;
  }
}

// ---- Pattern parsing + matching -----------------------------------------------------
struct Pattern {
  char net[8];
  char sta[8];
  char loc[8];
  char chan[8];
};

static bool parse_pattern(const char* in, Pattern& p) {
  char buf[SLINK_PATTERN_LEN];
  snprintf(buf, sizeof(buf), "%s", in);
  const char* fields[4] = {"*", "*", "*", "*"};
  uint8_t n = 0;
  char* save = NULL;
  for (char* t = strtok_r(buf, ".", &save); t && n < 4; t = strtok_r(NULL, ".", &save))
    fields[n++] = t;
  snprintf(p.net, sizeof(p.net), "%s", fields[0]);
  snprintf(p.sta, sizeof(p.sta), "%s", fields[1]);
  snprintf(p.loc, sizeof(p.loc), "%s", fields[2]);
  snprintf(p.chan, sizeof(p.chan), "%s", fields[3]);
  return n >= 3;   // NET.STA.LOC.CHAN expected; tolerate missing trailing field
}

static bool field_wild(const char* f) {
  return strpbrk(f, "?*") != NULL;
}

// '?' matches any single char; '*' matches any rest. Empty pattern field = '*'.
static bool field_match(const char* pat, const char* val) {
  if (!pat[0]) return true;
  while (*pat) {
    if (*pat == '*') return true;
    if (*pat == '?') {
      if (!*val) return false;
    } else if (*pat != *val) {
      return false;
    }
    pat++;
    if (*val) val++;
  }
  return *val == '\0';
}

static bool pattern_match(const Pattern& p, const MseedHead& h) {
  return field_match(p.net, h.net) && field_match(p.sta, h.sta) &&
         field_match(p.loc, h.loc) && field_match(p.chan, h.chan);
}

// SELECT token expansion (gaiacode-slink-rs expand_tau_selectors).
// Returns token count; 0 = stream unfiltered and match client-side.
static uint8_t expand_selects(const Pattern& p, char out[][6], uint8_t max) {
  if (field_wild(p.loc) || strlen(p.loc) != 2) return 0;
  if (!field_wild(p.chan)) {
    if (strlen(p.chan) != 3) return 0;
    snprintf(out[0], 6, "%s%s", p.loc, p.chan);
    return 1;
  }
  // Wildcarded channel: expand to the Z/N/E triad on a 2-char family prefix.
  char prefix[4] = "";
  for (uint8_t i = 0; i < 3 && p.chan[i] && p.chan[i] != '?' && p.chan[i] != '*'; i++) {
    prefix[i] = p.chan[i];
    prefix[i + 1] = '\0';
  }
  const char* family = prefix;
  if      (strcmp(prefix, "H") == 0) family = "HN";
  else if (strcmp(prefix, "B") == 0) family = "BN";
  else if (strcmp(prefix, "L") == 0) family = "LN";
  if (strlen(family) != 2) return 0;   // unknown band shape: don't guess
  const char comps[3] = {'Z', 'N', 'E'};
  uint8_t n = 0;
  for (uint8_t i = 0; i < 3 && n < max; i++)
    snprintf(out[n++], 6, "%s%s%c", p.loc, family, comps[i]);
  return n;
}

// ---- Lane management ---------------------------------------------------------------
static void lane_clear(Lane& l) {
  int32_t* keep = l.ring;        // memset must not orphan the PSRAM buffer
  memset(&l, 0, sizeof(l));
  l.ring = keep;
}

static int lane_for(const char* id) {
  for (uint8_t i = 0; i < s_lane_count; i++)
    if (strcmp(s_lanes[i].id, id) == 0) return i;
  if (s_lane_count >= SLINK_MAX_LANES) return -1;
  Lane& l = s_lanes[s_lane_count];
  lane_clear(l);
  snprintf(l.id, SLINK_ID_LEN, "%s", id);
  s_lane_count++;
  s_generation++;
  return s_lane_count - 1;
}

static void lanes_reset() {
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  s_lane_count = 0;
  s_overflow = 0;
  s_records = 0;
  s_matched = 0;
  for (uint8_t i = 0; i < SLINK_MAX_LANES; i++) lane_clear(s_lanes[i]);
  s_generation++;
  xSemaphoreGive(s_mtx);
}

static void lane_append(uint8_t lane, const int32_t* data, uint32_t n, float sps) {
  Lane& l = s_lanes[lane];
  if (!l.ring) return;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (l.sps == 0.0f) l.sps = sps;
  for (uint32_t i = 0; i < n; i++) {
    l.ring[l.w] = data[i];
    l.w = (l.w + 1) % SLINK_RING;
    if (l.count < SLINK_RING) l.count++;
  }
  l.total += n;
  l.records++;
  xSemaphoreGive(s_mtx);
}

// ---- Socket I/O ----------------------------------------------------------------------
static void set_error(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_error, sizeof(s_error), fmt, ap);
  va_end(ap);
  s_state = SL_ERROR;
  storage_log("SLINK", s_error);
}

// Read exactly n bytes with a millis deadline. Returns false on timeout/close.
static bool read_exact(WiFiClient& c, uint8_t* buf, size_t n, uint32_t deadline) {
  size_t got = 0;
  while (got < n) {
    if (s_disconnect_req) return false;
    int avail = c.available();
    if (avail > 0) {
      int r = c.read(buf + got, n - got);
      if (r <= 0) return false;
      got += r;
    } else if (!c.connected()) {
      return false;
    } else if ((int32_t)(millis() - deadline) >= 0) {
      return false;
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  return true;
}

// Read one CR/LF-terminated line (for handshake replies).
// Returns LR_OK / LR_CLOSED / LR_TIMEOUT / LR_ABORT for field diagnosis.
enum LineRead : uint8_t { LR_OK = 0, LR_CLOSED, LR_TIMEOUT, LR_ABORT };

static int read_line(WiFiClient& c, char* out, size_t len, uint32_t deadline) {
  size_t n = 0;
  while (n + 1 < len) {
    if (s_disconnect_req) return LR_ABORT;
    if (c.available()) {
      char ch = (char)c.read();
      if (ch == '\n') break;
      if (ch != '\r') out[n++] = ch;
    } else if (!c.connected()) {
      return LR_CLOSED;
    } else if ((int32_t)(millis() - deadline) >= 0) {
      return LR_TIMEOUT;
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  out[n] = '\0';
  return LR_OK;
}

static const char* lr_str(int r) {
  return r == LR_CLOSED ? "server closed" : (r == LR_TIMEOUT ? "timeout (no data)" : "aborted");
}

// ---- Connection run ---------------------------------------------------------------------
enum AttemptResult : uint8_t { AR_DONE, AR_RETRY, AR_ABORTED };

static AttemptResult run_attempt(uint16_t port) {
  Pattern pat;
  parse_pattern(s_pattern, pat);

  WiFiClient c;
  c.setTimeout(READ_TIMEOUT_MS / 1000);
  if (!c.connect(s_ip, port, CONN_TIMEOUT_MS)) {
    set_error("connect %s:%u failed (refused/unreachable)", s_ip, port);
    return AR_RETRY;
  }
  uint32_t deadline = millis() + READ_TIMEOUT_MS;

  // Segment separation: let the handshake ACK leave on its own before the
  // first command byte. lwIP can piggyback the connect ACK onto the first
  // data segment, and primitive embedded TCP stacks (Microchip on the Tau)
  // are known to drop/mishandle such segments — the working OmegaDesktop
  // capture shows ACK and HELLO as separate segments.
  vTaskDelay(pdMS_TO_TICKS(50));

  // HELLO -> drain TWO ident lines
  size_t wrote = c.print("HELLO\r\n");
  Serial.printf("SLINK: HELLO wrote %u/8 to %s:%u (local %s)\n",
                (unsigned)wrote, s_ip, port, WiFi.localIP().toString().c_str());
  char line[128];
  // Instrumented ident1 wait: report socket state every 2 s so the field
  // log shows whether bytes ever arrive and whether the link stays up.
  uint32_t t0 = millis();
  int r;
  for (;;) {
    r = read_line(c, line, sizeof(line), millis() + 2000);
    if (r != LR_TIMEOUT) break;
    Serial.printf("SLINK: ident1 wait: connected=%d avail=%d t=%lus\n",
                  (int)c.connected(), (int)c.available(),
                  (unsigned long)((millis() - t0) / 1000));
    if ((int32_t)(millis() - (t0 + READ_TIMEOUT_MS)) >= 0) break;
    if (s_disconnect_req) { r = LR_ABORT; break; }
  }
  if (r == LR_ABORT) { c.stop(); return AR_ABORTED; }
  if (r != LR_OK) {
    set_error("HELLO %s:%u: ident1 %s", s_ip, port, lr_str(r));
    c.stop();
    return AR_RETRY;   // maybe the wrong port (e.g. web port): try fallback
  }
  Serial.printf("SLINK: ident1: %s\n", line);
  r = read_line(c, line, sizeof(line), deadline);
  if (r == LR_ABORT) { c.stop(); return AR_ABORTED; }
  if (r != LR_OK) {
    set_error("HELLO %s:%u: ident2 %s", s_ip, port, lr_str(r));
    c.stop();
    return AR_RETRY;
  }
  Serial.printf("SLINK: ident2: %s\n", line);

  // SELECTs (explicit only); 0 = unfiltered stream, client-side match
  char selects[4][6];
  uint8_t nsel = expand_selects(pat, selects, 4);
  for (uint8_t i = 0; i < nsel; i++) {
    c.printf("SELECT %s\r\n", selects[i]);
    r = read_line(c, line, sizeof(line), deadline);
    if (r == LR_ABORT) { c.stop(); return AR_ABORTED; }
    if (r != LR_OK) {
      set_error("SELECT %s: %s", selects[i], lr_str(r));
      c.stop();
      return AR_DONE;   // handshake succeeded; this is the right port
    }
    if (strncmp(line, "ERROR", 5) == 0) {
      set_error("SELECT %s rejected: %s", selects[i], line);
      c.stop();
      return AR_DONE;
    }
  }

  // END -> no reply; streaming begins immediately
  c.print("END\r\n");
  s_state = SL_STREAMING;
  {
    char msg[96];
    snprintf(msg, sizeof(msg), "streaming %s:%u %s (%u SELECTs)",
             s_ip, port, s_pattern, nsel);
    storage_log("SLINK", msg);
  }

  static uint8_t frame[SL_FRAME];
  static int32_t samples[MAX_DECODE_SAMPLES];
  while (!s_disconnect_req) {
    deadline = millis() + READ_TIMEOUT_MS;
    if (!read_exact(c, frame, SL_FRAME, deadline)) break;

    // Signature check with 1-byte resync
    if (frame[0] != 'S' || frame[1] != 'L') {
      memmove(frame, frame + 1, SL_FRAME - 1);
      if (!read_exact(c, frame + SL_FRAME - 1, 1, deadline)) break;
      continue;
    }

    const uint8_t* rec = frame + 8;
    MseedHead h;
    parse_fsdh(rec, h);
    if (h.is_log) continue;
    s_records++;
    if (!pattern_match(pat, h)) continue;
    s_matched++;

    char id[SLINK_ID_LEN];
    snprintf(id, sizeof(id), "%s.%s.%s.%s", h.net, h.sta, h.loc, h.chan);
    int lane = lane_for(id);
    if (lane < 0) { s_overflow++; continue; }

    uint8_t word_order = 1;
    int encoding = find_b1000(rec, h, &word_order);
    if (encoding < 0 || word_order != 1) continue;   // BE only (TAU standard)
    if (h.data_offset < 48 || h.data_offset >= MSEED_REC) continue;

    int n = decode_payload(rec + h.data_offset, MSEED_REC - h.data_offset,
                           encoding, h.num_samples, samples, MAX_DECODE_SAMPLES);
    if (n > 0) lane_append((uint8_t)lane, samples, (uint32_t)n, h.sps);
  }

  c.stop();
  if (s_state == SL_STREAMING) {
    s_state = SL_IDLE;
    storage_log("SLINK", "disconnected");
  } else if (!s_disconnect_req) {
    // Streaming never started and no error was staged by the loop exit path.
    set_error("stream %s:%u ended before data", s_ip, port);
  }
  return AR_DONE;
}

static void run_connection() {
  lanes_reset();
  s_state = SL_CONNECTING;
  set_error("%s", "");

  // Advertised SeedLink port first, factory default as fallback (heartbeat
  // dialects differ in which port they carry on older firmware).
  uint16_t ports[2] = {s_port, 18000};
  for (uint8_t pi = 0; pi < 2 && !s_disconnect_req; pi++) {
    if (pi == 1 && ports[1] == ports[0]) break;
    AttemptResult ar = run_attempt(ports[pi]);
    if (ar != AR_RETRY) break;
  }
}

// ---- Worker task -------------------------------------------------------------------------
static void slink_task(void* arg) {
  (void)arg;
  for (;;) {
    if (s_connect_req) {
      s_connect_req = false;
      s_disconnect_req = false;
      run_connection();
      s_disconnect_req = false;
      continue;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
  }
}

// ---- Public API -----------------------------------------------------------------------------
void slink_init() {
  s_mtx = xSemaphoreCreateMutex();
  // Lane rings are large CPU-only buffers (never DMA'd): place them in PSRAM
  // and keep internal SRAM for the heap (WiFi/LWIP/SD/LVGL). Static-buffer
  // placement here previously starved the heap to ESP_ERR_NO_MEM + panic.
  for (uint8_t i = 0; i < SLINK_MAX_LANES; i++) {
    s_lanes[i].ring = (int32_t*)ps_malloc(SLINK_RING * sizeof(int32_t));
    if (!s_lanes[i].ring)
      s_lanes[i].ring = (int32_t*)malloc(SLINK_RING * sizeof(int32_t));
    if (!s_lanes[i].ring)
      Serial.printf("SLINK: !! lane %u ring allocation failed\n", i);
  }
  xTaskCreatePinnedToCore(slink_task, "slink", 8192, NULL, 1, &s_task, 0);
}

void slink_connect(const char* ip, uint16_t port, const char* pattern) {
  snprintf(s_ip, sizeof(s_ip), "%s", ip);
  s_port = port ? port : 18000;
  snprintf(s_pattern, sizeof(s_pattern), "%s", pattern);
  s_disconnect_req = true;   // stop any current stream first
  vTaskDelay(pdMS_TO_TICKS(50));
  s_connect_req = true;
  if (s_task) xTaskNotifyGive(s_task);
}

void slink_disconnect() {
  s_disconnect_req = true;
  s_state = SL_IDLE;
  if (s_task) xTaskNotifyGive(s_task);
}

const char* slink_state() {
  switch (s_state) {
    case SL_CONNECTING: return "connecting";
    case SL_STREAMING:  return "streaming";
    case SL_ERROR:      return "error";
    default:            return "idle";
  }
}

const char* slink_error()   { return s_error; }
uint32_t    slink_records() { return s_records; }
uint32_t    slink_matched() { return s_matched; }
uint8_t     slink_overflow(){ return s_overflow; }
const char* slink_pattern() { return s_pattern; }
uint32_t    slink_generation() { return s_generation; }

uint8_t slink_lanes(SlinkLaneInfo* out, uint8_t max) {
  if (!s_mtx) return 0;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  uint8_t n = s_lane_count < max ? s_lane_count : max;
  for (uint8_t i = 0; i < n; i++) {
    snprintf(out[i].id, SLINK_ID_LEN, "%s", s_lanes[i].id);
    out[i].sps = s_lanes[i].sps;
    out[i].total_samples = s_lanes[i].total;
    out[i].records = s_lanes[i].records;
  }
  xSemaphoreGive(s_mtx);
  return n;
}

uint32_t slink_read_lane(uint8_t lane, int32_t* out, uint32_t max, float* sps_out) {
  if (!s_mtx || lane >= s_lane_count) return 0;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  Lane& l = s_lanes[lane];
  uint32_t n = l.ring ? (l.count < max ? l.count : max) : 0;
  for (uint32_t i = 0; i < n; i++) {
    // oldest-first of the newest n samples
    uint32_t idx = (l.w + SLINK_RING - n + i) % SLINK_RING;
    out[i] = l.ring[idx];
  }
  if (sps_out) *sps_out = l.sps;
  xSemaphoreGive(s_mtx);
  return n;
}
