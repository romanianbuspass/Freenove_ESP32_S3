#!/usr/bin/env bash
# FNK0104N flash wrapper — esptool --no-stub @115200 (stub dies on USB-S/J).
# Uses PlatformIO's bundled esptool + penv python (venv-agnostic).
# Usage: flash_fnk0104n_nostub.sh <port> <build_dir>
set -e
PORT="${1:-/dev/ttyACM0}"
DIR="${2:?build dir}"
PY="$HOME/.local/share/pipx/venvs/platformio/bin/python"
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
"$PY" "$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 115200 \
  --before default_reset --after hard_reset --no-stub \
  write_flash -z --flash_mode qio --flash_freq 80m --flash_size 16MB \
  0x0     "$DIR/bootloader.bin" \
  0x8000  "$DIR/partitions.bin" \
  0x10000 "$DIR/firmware.bin"
