#pragma once
#include <Arduino.h>

// GaiaCode CYD — LAN discovery (Phase 3).
// Protocol ported from GaiaSignal_Rust tau_discovery.rs:
//   - Application Taus broadcast UDP heartbeats to port 5555 (passive listen):
//       GAIACODE:<DeviceID>:<WebPort>:<SeedLinkPort>   (format B, has hostname)
//       GIACODE<Port>:<hex> / !GAIACODE<Port>:<hex>    (format A, legacy)
//   - Bootloader devices answer an AN1388 query broadcast to port 6234:
//       [0x01, 0x01, CRC_L, CRC_H, 0x04]  (CRC-16/IBM over bytes 0..1)
// LVGL never touches this module directly: the UI pulls snapshots via
// net_snapshot() from its own timer.

struct LanDevice {
  char     ip[16];
  char     mac[18];        // MAC when known (self / format-A id), else "--"
  char     host[32];       // DeviceID, "Tau (bootloader)", or own hostname
  uint16_t sl_port;        // SeedLink port (app mode), 6234 boot, 0 self
  char     state[10];      // "app" | "boot"
};

void     net_init();                       // Wi-Fi up + discovery task (core 0)
void     net_scan();                       // trigger one on-demand Tau sweep
bool     net_connected();                  // STA has an IP
const char* net_state_str();               // "no wifi.cfg" | "connecting" | "scanning" | "connected"
uint8_t  net_snapshot(LanDevice* out, uint8_t max);   // copy device table
uint32_t net_generation();                 // bumps when the table changes
String   net_local_ip();                   // "0.0.0.0" when disconnected
