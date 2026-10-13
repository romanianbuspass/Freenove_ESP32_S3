#!/usr/bin/env python3
"""Reconnect-tolerant USB-CDC capture for the GaiaCode CYD.

The ESP32-S3 re-enumerates on every reset, so a plain serial handle goes
stale. This loops: wait for the node -> open -> stream -> on error, retry.
"""
import glob
import os
import sys
import time

import serial

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0
deadline = time.time() + DURATION

while time.time() < deadline:
    ports = glob.glob("/dev/ttyACM*")
    if not ports:
        time.sleep(0.3)
        continue
    port = ports[0]
    try:
        s = serial.Serial(port, 115200, timeout=0.5)
        sys.stdout.write(f"--- attached {port} ---\n")
        sys.stdout.flush()
        while time.time() < deadline:
            try:
                data = s.read(4096)
            except (serial.SerialException, OSError):
                break  # device went away; outer loop reattaches
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
        s.close()
    except (serial.SerialException, OSError) as e:
        sys.stdout.write(f"--- open failed: {e} ---\n")
        sys.stdout.flush()
        time.sleep(0.5)
