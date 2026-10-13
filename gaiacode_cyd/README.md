# GaiaCode CYD — Field Diagnostic Terminal

**Hardware:** Freenove FNK0104N (ESP32-S3R8, 3.5" ST77922 QSPI capacitive touch)
**Firmware stack:** Arduino-ESP32 2.0.14 / ESP-IDF 4.4, LVGL 8.4, FreeRTOS, PlatformIO

## 1. Project Overview

A unified, portable diagnostic and configuration terminal for GaiaCode seismic digitizers and early-warning infrastructure. One handheld device replaces a laptop in the field: it talks to sensors over UART, disciplines and validates GNSS timing, discovers Tau digitizers on the instrument LAN, and is being extended to reflash PIC targets via ICSP. Boot sequence: splash (2.5 s) → MD3 tabbed UI. Subsystems initialize in dependency order — HAL (display/touch/UART/activity LED) → audio (I2S codec) → storage (SD) → network → GNSS → UI — with a deferred SD status reprint to survive USB-CDC re-enumeration on the host.

## 2. User Interface (LVGL)

Single `lv_tabview` on a 320×480 portrait panel (40 px tab bar, 440 px content), styled with GaiaCode MD3 tokens (teal `#0EADB2` on dark navy) and the sensor group theme colors ported verbatim from the desktop `Sensor_Terminal.py`.

- **Sensor** — profile/baud selectors, dynamic command-button grid from static `SensorProfile` tables (Theta Pico / Theta / Sigma / Alpha), line-based RX auto-detect, and an 80 px bounded terminal readout. Commands replicate the desktop wake sequence (blank line, 150 ms, then command).
- **GNSS** — fix/SV status line, position/time/speed readout, six SNR bars (strongest SVs), 1 PPS lamp, and a 4-line raw NMEA console for on-the-wire diagnostics.
- **LAN** — on-demand Tau discovery with device cards (hostname, IP, MAC, SeedLink port, bootloader vs. application state).
- **ICSP** — SD-backed `.hex` file browser, target selector, and an armed FLASH TARGET binding. This is the preparation layer for the target configuration matrix (PIC32MZ / PIC24F / PIC16 / PIC18); the LVP protocol itself is intentionally pending.

Rendering correctness on the ST77922 is enforced in the HAL and treated as invariant: DMA-capable internal-SRAM draw buffers (never PSRAM), and a 4-px-aligned `rounder_cb` that eliminates QSPI pixel-stream shear on partial redraws.

## 3. GNSS & Timing Architecture

A dedicated FreeRTOS task owns UART2 (listen-only, 9600 baud, RX on IO46) so NMEA is never dropped during UI redraws. The UART RX ring is expanded to 2 KB to absorb full multi-constellation GSV bursts (~1.2 KB per epoch) that overflowed the 256-byte default. The parser is talker-ID agnostic — `$GP`, `$GL`, `$GN`, `$GB` all decode through the same handlers — covering GGA (fix, position, altitude), RMC (UTC, date, speed, course), GSA (fix dimension, DOPs, best-of across constellations), and GSV (per-constellation SV-in-view aggregation with per-PRN SNR, committed after a silence gap). Checksum tails are trimmed at `*` during field extraction. Lock state is a fusion of GGA quality and GSA fix type, with RMC position as fallback. A mutex-protected snapshot with a generation counter feeds the UI; the 1 PPS input on IO45 uses a rising-edge `IRAM_ATTR` ISR (`esp_timer_get_time()` + counter only) with `INPUT_PULLDOWN` — the idle-LOW clamp prevents crosstalk from the adjacent NMEA line and keeps the VDD_SPI strapping pin undisturbed. Pin routing deliberately avoids IO38/39, the live touch I2C bus.

## 4. DSP Audio & Haptics

Real-time procedural FM/PM synthesis — no `.wav` assets — rendered by a priority-2 FreeRTOS task in 256-frame chunks into the legacy I2S driver (44.1 kHz/16-bit, 2×1024-frame DMA, auto-clear on underrun) through the ES8311 codec (on the shared touch/audio I2C bus) and SC8002B amplifier (active-low enable). A 4-deep queue feeds three discrete-time phase-accumulator voices: **NAV_ZAP** (FM sweep with decaying modulation index) on navigation, **ACTION_KICK** (exponential pitch-drop, tanh-driven) on primary actions, and **PING_FAULT** (dual-detuned-carrier NeuroFunk growl) as the diagnostic alert — triggered when a sensor test report contains a ping/impulse-response fault token, and reused as the auditory radar ping on LAN scans. A legacy 8 ms percussive square burst provides immediate click haptics on the sensor matrix.

## 5. Network Operations

Wi-Fi STA with credentials from `/wifi.cfg` on SD (self-documenting template written on first boot, re-polled every 10 s). Discovery follows the GaiaCode Tau protocol — passive UDP heartbeat listening on `:5555` (`GAIACODE:` and legacy `GIACODE` formats) plus the AN1388 bootloader query broadcast to `:6234` (CRC-16/IBM), over two separate sockets — ported from `GaiaSignal_Rust/tau_discovery.rs`. Crucially, scanning is **on-demand and button-triggered**: the network task blocks on a task notification between sweeps, so there is zero background LAN chatter. Each sweep opens sockets, listens for ~3 s, then closes — preventing periodic broadcast storms from disrupting PIC32-based units during cold boot. One automatic sweep runs on first connect so the LAN tab is populated at startup; a successful contact confirms audibly.

## 6. Tau Configurator (Phase 5)

Tapping an application-state Tau card on the LAN tab opens a dedicated configurator screen (bootloader units and the CYD itself are not selectable — the AN1388 bootloader has no HTTP config server). Command grammar and transport are a full-parity port of the desktop Tau Bulk Configurator (`GaiaSignal_Rust/gaia-station-app`, `tau_config.rs` + `TauConfigurator.svelte`):

- **Transport** (`tau_config.cpp`): raw `WiFiClient` HTTP/1.1 with title-case headers — the Tau firmware's HTTP parser matches header names case-sensitively and silently ignores lowercase ones, so Arduino's HTTPClient is unsafe here. `POST /filters.htm` with form body `CMD=<cmd>`; handles both firmware dialects (legacy 302-redirect then GET, RESP in `<b>RESP:</b><td>`; Web_V3 RESP in the POST 200 body, `<td id="resp-row">`). ≥400 ms gap between commands, 5 s I/O timeout, fresh socket per call. Config read fetches `proto/filters/general/nvm1.json`, applies the `217c` PGA-token sanitizer, and flattens to key/value rows (FTP credentials hidden). A dedicated worker task (core 0) owns all sockets; LVGL enqueues jobs and polls mutex-protected, generation-countered snapshots — the UI thread never blocks on the network.
- **Screen** (`ui_tau_config.cpp`): single-scroll layout — Overview table first, then Sample Rate (≤4 rates/channel, Sensor A/B/All), PGA (gain 1/2/4/8/12 per channel or group), Network (dotted input converted to the firmware's space-separated grammar), Protocol (GAIA/SEED/GCF + SEED location/stream/station/network IDs), Aux (MAXTX/LLSPS/TCPFLUSH/EMODE/LOGRATE(H)/STATUSMODE(H) masks), Trigger (pre/post, HP/BP masks, STA/LTA/ratios, presets), Storage & GPS (DISSD/GPSX), Rotation (SETROT/raw ROTM), and a red-bordered Danger Zone isolated at the end (Re-Boot and FACTORY RESET behind typed-phrase confirmations; SAVE TO NVM sends `seed`). Every apply passes through a confirmation modal showing the exact CMD strings, then per-command RESP results stream into a Results card. Shared on-screen keyboard appears on textarea focus (numeric mode for numeric fields).

## 7. miniSEED Waveform Viewer (Phase 6)

The SEED tab is a live SeedLink client. The Tau picker is fed by the LAN scanner (application-state devices with their advertised SeedLink port); the selector is an editable `NET.STA.LOC.CHAN` pattern with `?`/`*` wildcards (default `XX.????.??.F??`). Connection protocol is ported from `gaiacode-slink-rs` and the `gaia-tau-seedlink-protocol` knowledge base:

- **Handshake** (`slink.cpp`): the GAIA TAU embedded server is uni-station — never `STATION`, never `*` on the wire. `HELLO` (drain both ident lines) → explicit `SELECT <LOCCHAN>` tokens → `END` (no reply; streaming starts immediately). SELECT expansion mirrors `expand_tau_selectors` (exact `00HNZ`, or the Z/N/E triad on a normalised family prefix like `HN?`→HNZ/HNN/HNE). When the pattern's loc/chan wildcards can't be expanded (e.g. the default `??.F??`), **no SELECTs are sent** — the TAU streams its full ring buffer and each record's `NET.STA.LOC.CHAN` is matched client-side, the documented reliable fallback.
- **Framing**: 520-byte frames (8-byte ASCII `SLnnnnnn` header + 512-byte miniSEED record) with 1-byte resync on bad signatures; LOG-channel keepalives skipped.
- **Decoding**: FSDH + Blockette-1000 chain walk; Steim1/Steim2 decompression ported from `gaia_dsp/src/steim.rs` with exact libmseed frame semantics (frame-0 W1 = X0 is the first sample, W2 = Xn check-only, data from W3 with the cross-record continuity diff skipped; continuation frames W1–W15 all data, chained). Uncompressed int16/int32/float32/float64 also supported; big-endian records only (TAU standard).
- **Display**: up to 4 matched streams get lanes (extras counted as overflow). Each lane keeps a 4096-sample int32 ring on the worker task; the UI decimates the selected window (10 s / 30 s / 60 s / 5 min) to a per-column mean polyline, auto-scaled to the window peak (shown in the lane header with the measured sample rate). A dedicated core-0 task owns the socket; LVGL only polls mutex-protected snapshots.

Record times use each record's real BTIME start; the ring is append-only, so clock gaps appear as stitched data (acceptable for a field viewer; gap tracking is a desktop concern).
