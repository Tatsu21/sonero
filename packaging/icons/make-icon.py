#!/usr/bin/env python3
"""Render the Sonero icons.

Writes square PNGs (no third-party deps: the PNG is encoded by hand with zlib).

Two variants, because a launcher icon and a panel icon have different jobs:

  sonero-<N>.png       the app icon — a dark rounded tile with three mixer
                           faders, shown at 32px and up in menus and docks.
  sonero-tray-<N>.png  the system-tray icon for Cinnamon, KDE, XFCE. Drawn
                           at 16-48px on a panel whose colour we cannot know, so
                           it has no background tile and uses thicker strokes in
                           mid-tones that stay legible on light *and* dark panels.

Usage:  python3 make-icon.py [output_dir]
"""

import math
import os
import struct
import sys
import zlib

SS = 4  # supersampling factor, downsampled at the end for anti-aliasing

BG_TOP = (0x1E, 0x21, 0x30)
BG_BOTTOM = (0x11, 0x12, 0x19)
TRACK = (0x2C, 0x31, 0x48)
# Mid-tone track for the tray icon: dark enough to read on a light panel, light
# enough to read on a dark one.
TRAY_TRACK = (0x7C, 0x83, 0x96)

FADERS = [  # (x_centre_ratio, knob_y_ratio, colour)
    (0.285, 0.62, (0x7A, 0xA2, 0xF7)),
    (0.500, 0.38, (0x9E, 0xCE, 0x6A)),
    (0.715, 0.55, (0xF7, 0x76, 0x8E)),
]


def rounded_rect_alpha(x, y, w, h, r):
    """Coverage (0/1) of a rounded rectangle at point (x, y)."""
    if x < 0 or y < 0 or x >= w or y >= h:
        return 0.0
    cx = min(max(x, r), w - r)
    cy = min(max(y, r), h - r)
    dx, dy = x - cx, y - cy
    return 1.0 if dx * dx + dy * dy <= r * r else 0.0


def render(size, tray=False):
    """Render one icon, returning raw PNG scanlines (RGBA, filter byte per row)."""
    n = size * SS

    if tray:
        # No tile: thicker tracks and bigger knobs so the shape survives at 16px.
        radius = 0.0
        track_w = n * 0.085
        track_top, track_bottom = n * 0.14, n * 0.86
        knob_r = n * 0.105
        track_colour = TRAY_TRACK
    else:
        radius = n * 0.22
        track_w = n * 0.055
        track_top, track_bottom = n * 0.235, n * 0.765
        knob_r = n * 0.072
        track_colour = TRACK

    rows = []
    for py in range(n):
        row = []
        for px in range(n):
            x, y = px + 0.5, py + 0.5

            if tray:
                colour = None  # transparent until something is drawn
            else:
                if rounded_rect_alpha(x, y, n, n, radius) <= 0.0:
                    row.append((0, 0, 0, 0))
                    continue
                t = y / n
                colour = tuple(
                    int(round(BG_TOP[i] + (BG_BOTTOM[i] - BG_TOP[i]) * t)) for i in range(3)
                )

            for fx, fy, accent in FADERS:
                cx = n * fx
                # Fader track (a vertical capsule).
                if abs(x - cx) <= track_w / 2 and track_top <= y <= track_bottom:
                    colour = track_colour
                else:
                    for end_y in (track_top, track_bottom):
                        if math.hypot(x - cx, y - end_y) <= track_w / 2:
                            colour = track_colour
                # Knob.
                if math.hypot(x - cx, y - n * fy) <= knob_r:
                    colour = accent

            row.append((0, 0, 0, 0) if colour is None else colour + (255,))
        rows.append(row)

    # Box-downsample to the requested size.
    out = []
    for oy in range(size):
        line = bytearray()
        line.append(0)  # PNG filter type 0
        for ox in range(size):
            r = g = b = a = 0
            for sy in range(SS):
                for sx in range(SS):
                    pr, pg, pb, pa = rows[oy * SS + sy][ox * SS + sx]
                    r += pr * pa
                    g += pg * pa
                    b += pb * pa
                    a += pa
            if a == 0:
                line += bytes((0, 0, 0, 0))
            else:
                line += bytes((r // a, g // a, b // a, a // (SS * SS)))
        out.append(bytes(line))
    return b"".join(out)


def write_png(path, size, tray=False):
    raw = render(size, tray)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)
    print(f"wrote {os.path.basename(path)} ({size}x{size}, {len(png)} bytes)")


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    os.makedirs(out_dir, exist_ok=True)

    for size in (256, 128, 64, 48, 32):
        write_png(os.path.join(out_dir, f"sonero-{size}.png"), size)
    # The canonical icon the desktop entry refers to.
    write_png(os.path.join(out_dir, "sonero.png"), 256)

    # Panel sizes actually used by trays: 16 and 22-24 are the common ones.
    for size in (16, 22, 24, 32, 48):
        write_png(os.path.join(out_dir, f"sonero-tray-{size}.png"), size, tray=True)


if __name__ == "__main__":
    main()
