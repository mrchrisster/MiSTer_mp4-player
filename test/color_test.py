#!/usr/bin/env python3
"""
Color palette test for MiSTer ASCAL framebuffer at 0x30000000.

Hypothesis: framebuffer uses RBG565 big-endian layout:
  bits [15:11] = Red   (5 bits)
  bits [10:5]  = Blue  (6 bits)   <- swapped vs standard RGB565
  bits [4:0]   = Green (5 bits)   <- swapped vs standard RGB565

RBG565BE pixel packing:
  pixel = ((r >> 3) << 11) | ((b >> 2) << 5) | (g >> 3)
  write big-endian: [pixel >> 8, pixel & 0xFF]

Run:
  python3 color_test.py        -> paint 8-color palette (all primaries + secondaries)
  python3 color_test.py <0-7>  -> paint one solid color
"""

import mmap, os, sys, struct

FB_ADDR = 0x30000000
FB_W    = 640
FB_H    = 480
FB_SIZE = FB_W * FB_H * 2

def rbg565be(r, g, b):
    """Pack RGB (0-255 each) into RBG565 big-endian bytes."""
    pixel = ((r & 0xF8) << 8) | ((b & 0xFC) << 3) | (g >> 3)
    return bytes([pixel >> 8, pixel & 0xFF])

# 8 color bands — primaries + secondaries
BANDS = [
    ("BLACK",   rbg565be(  0,   0,   0)),
    ("RED",     rbg565be(255,   0,   0)),
    ("GREEN",   rbg565be(  0, 255,   0)),
    ("BLUE",    rbg565be(  0,   0, 255)),
    ("YELLOW",  rbg565be(255, 255,   0)),
    ("CYAN",    rbg565be(  0, 255, 255)),
    ("MAGENTA", rbg565be(255,   0, 255)),
    ("WHITE",   rbg565be(255, 255, 255)),
]

def open_fb():
    fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
    m  = mmap.mmap(fd, FB_SIZE, mmap.MAP_SHARED,
                   mmap.PROT_READ | mmap.PROT_WRITE, offset=FB_ADDR)
    return fd, m

def paint_all_bands():
    fd, m = open_fb()
    rows_per_band = FB_H // len(BANDS)
    for i, (label, pixel) in enumerate(BANDS):
        offset = i * rows_per_band * FB_W * 2
        count  = rows_per_band * FB_W
        m.seek(offset)
        m.write(pixel * count)
        print(f"Band {i}: {label}  bytes={pixel.hex()}")
    m.close()
    os.close(fd)
    print("\nExpected top-to-bottom: BLACK RED GREEN BLUE YELLOW CYAN MAGENTA WHITE")
    print("Report any bands that show the wrong color.")

def paint_solid(idx):
    label, pixel = BANDS[idx]
    fd, m = open_fb()
    m.seek(0)
    m.write(pixel * (FB_W * FB_H))
    m.close()
    os.close(fd)
    print(f"Solid: {label}  bytes={pixel.hex()}")

if __name__ == '__main__':
    if len(sys.argv) > 1:
        paint_solid(int(sys.argv[1]))
    else:
        paint_all_bands()
