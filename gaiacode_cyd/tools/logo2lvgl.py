#!/usr/bin/env python3
"""Convert Gaiacode.png to an LVGL 8.x C array (LV_IMG_CF_TRUE_COLOR_ALPHA).

Output: src/gaiacode_logo.c + include/gaiacode_logo.h
Target width fits the 320-px-wide ST77922 with margin.
"""
from PIL import Image
import os

SRC = "/home/sysop/.hermes/Workspace/branding/Gaiacode.png"
OUT_C = "src/gaiacode_logo.c"
OUT_H = "include/gaiacode_logo.h"
TARGET_W = 280  # px; 320-wide panel, 20 px margin each side

img = Image.open(SRC).convert("RGBA")
ratio = TARGET_W / img.width
img = img.resize((TARGET_W, round(img.height * ratio)), Image.LANCZOS)
w, h = img.size

# LVGL 8 LV_IMG_CF_TRUE_COLOR_ALPHA: per pixel = RGB565 (little-endian) + alpha byte
data = bytearray()
for y in range(h):
    for x in range(w):
        r, g, b, a = img.getpixel((x, y))
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        data.append(rgb565 & 0xFF)
        data.append(rgb565 >> 8)
        data.append(a)

def fmt(data):
    lines = []
    for i in range(0, len(data), 12):
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in data[i:i+12]) + ",")
    return "\n".join(lines)

c = f"""/* Auto-generated from Gaiacode.png ({w}x{h}) — LVGL 8 LV_IMG_CF_TRUE_COLOR_ALPHA */
#include <lvgl.h>

static const uint8_t gaiacode_logo_map[] = {{
{fmt(data)}
}};

const lv_img_dsc_t gaiacode_logo = {{
  .header = {{
    .cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
    .always_zero = 0,
    .reserved = 0,
    .w = {w},
    .h = {h},
  }},
  .data_size = {len(data)},
  .data = gaiacode_logo_map,
}};
"""

hdr = """#pragma once
#include <lvgl.h>
extern const lv_img_dsc_t gaiacode_logo;
"""

os.makedirs("src", exist_ok=True)
os.makedirs("include", exist_ok=True)
open(OUT_C, "w").write(c)
open(OUT_H, "w").write(hdr)
print(f"{OUT_C}: {w}x{h}, {len(data)} bytes ({len(data)/1024:.1f} KiB)")
