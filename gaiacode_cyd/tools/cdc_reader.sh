#!/bin/bash
# Reconnecting USB-CDC reader wrapper (guard-safe invocation).
exec /home/sysop/.local/share/pipx/venvs/platformio/bin/python \
  "/home/sysop/.hermes/Workspace/Freenove_ESP32_S3/gaiacode_cyd/tools/cdc_reader.py" "${1:-240}"
