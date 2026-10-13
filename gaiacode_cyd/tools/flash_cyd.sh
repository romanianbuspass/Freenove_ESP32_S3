#!/bin/bash
# Flash GaiaCode CYD firmware via esptool directly (no reset dance — board is
# already in ROM download mode; pio's 1200bps touch would kick it back out).
set -e
PYHOME="$HOME/.local/share/pipx/venvs/platformio/bin/python"
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy"
BUILD="/home/sysop/.hermes/Workspace/Freenove_ESP32_S3/gaiacode_cyd/.pio/build/fnk0104n"
PORT="${1:-/dev/ttyACM0}"

cd "$ESPTOOL"
export PYTHONPATH="$ESPTOOL"

"$PYHOME" -m esptool --chip esp32s3 --port "$PORT" --baud 460800 \
  --before no_reset --after hard_reset \
  write_flash -z --flash_mode qio --flash_freq 80m --flash_size 16MB \
  0x0     "$BUILD/bootloader.bin" \
  0x8000  "$BUILD/partitions.bin" \
  0x10000 "$BUILD/firmware.bin"
