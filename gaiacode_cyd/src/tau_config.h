#pragma once
#include <Arduino.h>

// GaiaCode CYD — Tau HTTP configurator transport (Phase 5).
// Port of GaiaSignal_Rust/gaia-station-app/src-tauri/src/tau_config.rs:
//   - Commands: POST /filters.htm with form body CMD=<cmd> over a raw
//     HTTP/1.1 socket with TITLE-CASE headers (the Tau firmware's HTTP
//     parser matches header names case-sensitively and silently ignores
//     lowercase ones — Arduino HTTPClient is therefore unsafe here).
//   - Two firmware dialects: legacy 302-redirect (POST -> 302 -> GET page,
//     RESP in <b>RESP:</b><td>) and Web_V3 (RESP in the POST 200 body,
//     <td id="resp-row">).
//   - >=400 ms gap between commands; 5 s I/O timeout; fresh socket per call.
//   - Config read: GET proto/filters/general/nvm1 .json, flattened to
//     key/value rows (PGA "217c" token sanitization applied).
// A dedicated worker task (core 0) owns all sockets; LVGL enqueues jobs and
// polls snapshots — the UI thread never blocks on the network.

#define TAU_MAX_CMDS   24    // factory reset = 22 commands
#define TAU_CMD_LEN    80    // quoted 8-value trigger lists fit
#define TAU_LOG_LINES  16
#define TAU_LOG_LEN    128
#define TAU_ROWS_MAX   72
#define TAU_ROW_LEN    96

void tau_config_init();   // create worker task; call once from setup()

// Enqueue a command batch for one Tau (1..TAU_MAX_CMDS, sent sequentially
// with 400 ms gaps). All commands go to /filters.htm, mirroring the desktop.
// Returns false when the queue is full or a command exceeds TAU_CMD_LEN-1.
bool tau_send_batch(const char* ip, const char cmds[][TAU_CMD_LEN], uint8_t n);
bool tau_send_one(const char* ip, const char* cmd);

// Enqueue a config fetch (proto/filters/general/nvm1 -> flattened rows).
bool tau_fetch(const char* ip);

// ---- UI snapshots (mutex-protected, generation-countered) -----------------
bool     tau_busy();                    // a job is in flight
const char* tau_status();               // human-readable worker state
uint32_t tau_log_generation();          // bumps on each appended result line
uint8_t  tau_log_snapshot(char out[][TAU_LOG_LEN], uint8_t max);  // oldest..newest
uint32_t tau_rows_generation();         // bumps when the overview rows refresh
uint8_t  tau_rows_snapshot(char out[][TAU_ROW_LEN], uint8_t max);
