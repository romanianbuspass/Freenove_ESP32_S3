#pragma once
#include <Arduino.h>

// GaiaCode CYD storage backbone — FNK0104N internal SD slot.
// SDMMC: CMD=4, CLK=5, D0=6, D1=7, D2=2, D3=3 (board JSON authoritative).
// Mounts FAT32 at /sdcard, prints card report to the USB-CDC console,
// and guarantees the /firmware and /logs root directories exist.

bool     storage_init();        // call once from setup(); safe if no card
void     storage_report();      // (re)print card size/mount status to USB-CDC
bool     storage_mounted();     // true after a successful mount
// List *.hex files in /firmware (names truncated to 39 chars). Returns count.
uint8_t  storage_list_hex(char names[][40], uint8_t max);
// Append one timestamped line to /logs/cyd.log (no-op when unmounted).
void     storage_log(const char* tag, const char* msg);
// Read Wi-Fi credentials from /wifi.cfg; writes a template if absent.
// Returns false when missing, unreadable, or still placeholder values.
bool     storage_wifi_creds(char* ssid, size_t ssid_len, char* pass, size_t pass_len);
uint64_t storage_card_size_mb();
uint64_t storage_used_bytes();
