#!/bin/bash
# Reconnect-tolerant serial capture for the CYD (wrapper for cyd_capture.py).
DUR="${1:-90}"
~/.local/share/pipx/venvs/platformio/bin/python \
  "/home/sysop/.hermes/Workspace/Freenove_ESP32_S3/gaiacode_cyd/tools/cyd_capture.py" "$DUR" \
  > /tmp/cyd_capture.log 2>&1
echo "capture done" >> /tmp/cyd_capture.log
