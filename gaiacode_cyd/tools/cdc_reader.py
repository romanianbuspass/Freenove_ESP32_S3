#!/usr/bin/env python3
"""Reconnecting USB-CDC reader for native-USB ESP32-S3 boards.
Survives re-enumeration: scans ttyACM*, opens whatever exists, reads until
the deadline, reopening on disconnect."""
import glob, sys, time
import serial

deadline = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 180)
buf = b""
while time.time() < deadline:
    ports = sorted(glob.glob("/dev/ttyACM*"))
    if not ports:
        time.sleep(0.3)
        continue
    port = ports[-1]
    try:
        with serial.Serial(port, 115200, timeout=0.5) as s:
            print(f"[opened {port}]", flush=True)
            while time.time() < deadline:
                data = s.read(256)
                if data:
                    sys.stdout.write(data.decode("utf-8", "replace"))
                    sys.stdout.flush()
    except (serial.SerialException, OSError) as e:
        print(f"[{port} lost: {e}]", flush=True)
        time.sleep(0.3)
print("[done]", flush=True)
