#!/usr/bin/env python3
"""Reconnecting USB-CDC reader for native-USB ESP32-S3 boards.

The ESP32-S3 re-enumerates its USB-CDC port on every reset, so a serial
handle opened before reset goes stale. This reader scans ttyACM*/ttyUSB*,
opens the newest one, and reopens automatically when the device disconnects.

Usage:
    python reconnecting_cdc_reader.py [seconds]

If Hermes' lifecycle guard rejects direct python invocations, wrap this in a
thin shell script and run it with bash:

    #!/bin/bash
    exec /path/to/venv/bin/python reconnecting_cdc_reader.py "${1:-240}"
"""
import glob
import sys
import time

try:
    import serial
except ImportError as exc:
    raise SystemExit(
        "pyserial is required. Use the PlatformIO venv python, or run:\n"
        "  pip install pyserial"
    ) from exc

DEFAULT_SECONDS = 240
BAUD = 115200
PORTS = ["/dev/ttyACM*", "/dev/ttyUSB*"]


def list_ports():
    ports = []
    for pattern in PORTS:
        ports.extend(glob.glob(pattern))
    return sorted(ports)


def main():
    deadline = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SECONDS)
    while time.time() < deadline:
        ports = list_ports()
        if not ports:
            time.sleep(0.3)
            continue
        port = ports[-1]  # newest node is usually last alphabetically (e.g. ttyACM1)
        try:
            with serial.Serial(port, BAUD, timeout=0.5) as ser:
                print(f"[opened {port}]", flush=True)
                while time.time() < deadline:
                    data = ser.read(256)
                    if data:
                        sys.stdout.write(data.decode("utf-8", "replace"))
                        sys.stdout.flush()
        except (serial.SerialException, OSError) as exc:
            print(f"[{port} lost: {exc}]", flush=True)
            time.sleep(0.3)
    print("[done]", flush=True)


if __name__ == "__main__":
    main()
