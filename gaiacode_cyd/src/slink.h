#pragma once
#include <Arduino.h>

// GaiaCode CYD — SeedLink waveform client (Phase 6).
// Connection model ported from gaiacode-slink-rs + the gaia-tau-seedlink-protocol
// knowledge base (GaiaSignal_Rust):
//   - GAIA TAU embedded server is UNI-STATION: never send STATION / `*`.
//   - Handshake: HELLO (drain TWO ident lines) -> SELECT <LOCCHAN> per
//     explicit channel -> END (no reply; streaming begins immediately).
//   - SELECT only works with explicit loc+chan tokens; when the user's
//     SEED pattern contains wildcards in loc/chan that cannot be expanded,
//     send NO selects — the TAU then streams its full ring buffer and we
//     match `NET.STA.LOC.CHAN` from each record header client-side (the
//     documented reliable fallback).
//   - Framing: 520-byte frames = 8-byte ASCII `SLnnnnnn` header + 512-byte
//     miniSEED record. LOG-channel payloads are keepalives; skipped.
//   - Payloads: Steim1/Steim2 (libmseed frame semantics, ported from
//     gaia_dsp/src/steim.rs) plus uncompressed int16/int32/float/double.
//
// A dedicated task (core 0) owns the socket; LVGL polls snapshots and reads
// lane ring buffers under a mutex — the UI thread never blocks on TCP.

#define SLINK_MAX_LANES 4
#define SLINK_RING      4096    // int32 samples per lane
#define SLINK_ID_LEN    24      // "NET.STA.LOC.CHAN" + NUL
#define SLINK_PATTERN_LEN 40

struct SlinkLaneInfo {
  char     id[SLINK_ID_LEN];
  float    sps;
  uint32_t total_samples;   // since connect
  uint32_t records;
};

void slink_init();   // worker task; call once from setup()

// Begin a connection (replaces any active one). `pattern` is
// "NET.STA.LOC.CHAN" with optional ? / * wildcards, e.g. "XX.????.??.F??".
void slink_connect(const char* ip, uint16_t port, const char* pattern);
void slink_disconnect();

const char* slink_state();        // "idle" | "connecting" | "streaming" | "error"
const char* slink_error();        // last error detail ("" when none)
uint32_t    slink_records();      // total miniSEED records seen this connection
uint32_t    slink_matched();      // records matching the pattern
uint8_t     slink_overflow();     // matched streams beyond SLINK_MAX_LANES
const char* slink_pattern();

// Lane snapshot. Returns lane count; fills `out` (up to max).
uint8_t slink_lanes(SlinkLaneInfo* out, uint8_t max);
uint32_t slink_generation();      // bumps when the lane set changes

// Copy the newest `max` samples of a lane (oldest-first). Returns count.
// `sps_out` receives the lane's measured sample rate.
uint32_t slink_read_lane(uint8_t lane, int32_t* out, uint32_t max, float* sps_out);
