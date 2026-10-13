#pragma once
#include <Arduino.h>

// GaiaCode CYD — GNSS interpreter + 1 PPS timing.
// UART2 listens to the GNSS module; a rising-edge ISR on the PPS pin
// timestamps the pulse with esp_timer_get_time().
//
// Pin routing (see project note — IO38/IO39 are the touch I2C bus and were
// rejected for GNSS; extended-IO header used instead):
//   GNSS_RX_PIN  46   (module TX -> CYD RX; UART idles HIGH — safe for the
//                      IO46 ROM-boot-message strapping pin)
//   GNSS_TX_PIN  -1   (unassigned — listen-only per brief)
//   GNSS_PPS_PIN 45   (1 PPS, rising edge; idles LOW — safe for the IO45
//                      VDD_SPI strapping pin)

#define GNSS_RX_PIN   46
#define GNSS_TX_PIN   -1
#define GNSS_PPS_PIN  45
#define GNSS_BAUD     9600   // GNSS module default — change here if needed

struct GnssSat {
  uint8_t prn;
  uint8_t snr;    // dB-Hz, 0 = not tracked
};

struct GnssState {
  bool     locked;        // GGA quality > 0 OR GSA fix type >= 2
  uint8_t  fix_quality;   // GGA quality: 0=none 1=GPS 2=DGPS 4=RTK 5=RTK float
  uint8_t  sats_used;     // GGA satellites in use
  uint8_t  sats_in_view;  // GSV satellites in view
  double   lat, lon;      // decimal degrees
  float    alt_m;
  uint32_t gga_count;     // sentences seen (diagnostic)
  uint32_t gsv_cycles;    // completed GSV sequences
  // RMC — recommended minimum: time/date, speed, course, validity
  bool     rmc_valid;     // RMC status 'A'
  char     utc[11];       // hhmmss.ss as received
  char     date[7];       // ddmmyy as received
  float    speed_kn;
  float    course_deg;
  uint32_t rmc_count;     // sentences seen (diagnostic)
  // GSA — fix mode + DOPs (multi-constellation: best fix wins)
  char     gsa_mode;      // 'A' automatic / 'M' manual, 0 = none seen
  uint8_t  gsa_fix;       // 1=no fix 2=2D 3=3D (max across talkers)
  float    pdop, hdop, vdop;
  uint32_t gsa_count;     // sentences seen (diagnostic)
};

void     gnss_init();                          // UART2 + PPS ISR + parser task
void     gnss_snapshot(GnssState* out);
uint8_t  gnss_top_sats(GnssSat* out, uint8_t max);   // strongest SNR first
uint32_t gnss_generation();                    // bumps on new GGA/RMC/GSA/GSV data

// 1 PPS edge counters (updated from ISR context; 32-bit reads are atomic).
uint32_t gnss_pps_count();
int64_t  gnss_pps_last_us();

// Raw NMEA console (last 4 sentences as received, for the GNSS tab).
uint32_t gnss_raw_generation();                        // bumps per sentence
void     gnss_raw_snapshot(char* out, size_t len);     // oldest-first, \n-joined
