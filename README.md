# Freenove ESP32-S3 Workspace

Firmware projects and reference material for the **Freenove ESP32-S3 Display** boards
(FNK0104 family — ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM, QSPI colour LCD with
capacitive touch, ES8311 audio codec, WS2812 RGB LED, SD card slot, battery monitor).

## Contents

| Directory | Origin | Description |
|---|---|---|
| `gaiacode_cyd/` | **Custom project** | GaiaCode CYD — field diagnostic & configuration terminal for GaiaCode seismic digitizers (see `gaiacode_cyd/README.md` for full architecture docs) |
| `Launcher_Freenove-main/` | Third-party (public) | [M5Launcher](https://github.com/bmorcelli/M5Stick-Launcher) fork adapted for the Freenove ESP32-S3 display — OTA/SD app launcher |
| `Freenove_ESP32_S3_Display-main/` | Vendor (public) | Freenove's official tutorial tree: Arduino sketches (touch / no-touch / Xiaozhi AI variants), schematics, board photos |
| `esp32/` | Reference | Board/peripheral interface screenshots |
| `Logs/` | Runtime logs | Sanitized serial logs from the GaiaCode CYD (boot, LAN scans, GNSS lock events) |

## GaiaCode CYD (custom firmware)

A portable LVGL-based diagnostic terminal for GaiaCode Tau seismic digitizers:
UART sensor console, GNSS timing/1 PPS validation, on-demand LAN discovery
(UDP heartbeat `:5555` + AN1388 bootloader query `:6234`), Tau HTTP configurator,
live miniSEED/SeedLink waveform viewer (Steim1/2 decompression), and procedural
FM audio haptics. Target board: Freenove FNK0104N (3.5" ST77922 QSPI, 320×480).

- **Stack:** Arduino-ESP32 2.0.14 / ESP-IDF 4.4, LVGL 8.4, ArduinoJson 6.21.5, PlatformIO (`espressif32@6.7.0`, `esp32-s3-devkitc-1` + N16R8/octal-PSRAM overrides)
- **Build & flash:** `pio run -e fnk0104n -t upload` (see `gaiacode_cyd/tools/` for CDC log capture and no-stub flash scripts)
- **Wi-Fi:** credentials are read from `/wifi.cfg` on the SD card (a self-documenting template is written on first boot) — no credentials in the repo

## Technology stack (overall)

- **MCU:** Espressif ESP32-S3R8 (Xtensa dual-core, 16 MB QIO flash, 8 MB OPI PSRAM)
- **Frameworks:** Arduino-ESP32, ESP-IDF (via PlatformIO), FreeRTOS
- **UI/DSP:** LVGL 8.4, TFT_eSPI, ESP32-audioI2S, legacy I2S driver (ES8311 codec)
- **Protocols:** SeedLink/miniSEED (Steim1/Steim2), NMEA-0183 GNSS, UDP discovery, HTTP/1.1 config, BLE, Wi-Fi

## Hardware dependencies

- Freenove ESP32-S3 Display board (FNK0104N touch / FNK0104S / FNK0104AB variants)
- Onboard: ST77922 QSPI LCD + FT6336G touch, ES8311 codec + SC8002B amp, WS2812, SD slot
- Optional externals (GaiaCode CYD): GNSS module on UART2 (RX IO46, 1 PPS on IO45), sensor on UART0, SD card ≥ 4 GB

## Setup & installation

1. Install [PlatformIO](https://platformio.org/) (or Arduino IDE 2.x with the ESP32 core for the tutorial sketches).
2. **GaiaCode CYD:** `cd gaiacode_cyd && pio run -e fnk0104n -t upload`
3. **Launcher:** `cd Launcher_Freenove-main/Launcher_Freenove-main && pio run -t upload`
4. **Tutorial sketches:** open any `Freenove_ESP32_S3_Display-main/**/Sketches/Sketch_*/` `.ino` in the Arduino IDE, select the ESP32S3 Dev Module board (16 MB flash, OPI PSRAM), and upload.

## Excluded vendor material (re-download from Freenove)

The following bulk vendor files are intentionally **not** in this repo (size/redistribution);
they are available from Freenove's official product page and GitHub
(`Freenove/Freenove_ESP32_S3_Display`):

- `Tutorial.pdf` and the XiaoZhi AI extended tutorial PDFs
- `Datasheet/*.pdf` (ESP32-S3 TRM, ST77922, ES8311, FT6336G, etc.)
- `Libraries/*.zip` (lvgl 8.4.0, ESP32-audioI2S 3.0.13, TFT_eSPI 2.5.43 — also installable via the Arduino Library Manager / PlatformIO registry)
- `Freenove Image Tool.exe` (Windows image converter)
- `Upload_Xiaozhi_Bin/*/bin/*.bin` (prebuilt XiaoZhi AI firmware)
- `Sketches/*/music/*.mp3` (demo audio assets)
- PlatformIO build output (`.pio/`) and network packet captures (`*.pcapng`)

## Data formats / protocols

- **SeedLink:** 520-byte frames (`SLnnnnnn` header + 512-byte miniSEED record), FSDH + Blockette-1000, Steim1/Steim2 compression, big-endian
- **GNSS:** NMEA-0183 (GGA/RMC/GSA/GSV), talker-ID agnostic, 9600 baud
- **Tau discovery:** UDP `:5555` heartbeats (`GAIACODE:` / legacy `GIACODE:`), AN1388 bootloader broadcast `:6234` (CRC-16/IBM)
- **Tau config:** HTTP/1.1 `POST /filters.htm`, form body `CMD=<cmd>`, title-case headers (firmware parser is case-sensitive)

## Sanitization note

This is a sanitized upload: internal IPs, GNSS deployment coordinates, and device
serial IDs in runtime logs were replaced with `<INTERNAL_IP>`, `<REDACTED_LOCATION>`,
and `<SERIAL_NUMBER>` placeholders. No credentials, keys, or tokens are present.
