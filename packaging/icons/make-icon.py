#!/usr/bin/env python3
"""Render the LinuxSonar application icon.

Writes square PNGs (no third-party deps: the PNG is encoded by hand with zlib).
The artwork is a dark rounded tile with three mixer faders in the app's accent
colours — readable down to 32x32, which is what desktops use in tray/task lists.

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


def blend(dst, src, alpha):
    return tuple(int(round(d + (s - d) * alpha)) for d, s in zip(dst, src))


def render(size):
    n = size * SS
    # Geometry in supersampled space.
    radius = n * 0.22
    track_w = n * 0.055
    track_top, track_bottom = n * 0.235, n * 0.765
    knob_r = n * 0.072

    rows = []
    for py in range(n):
        row = []
        for px in range(n):
            x, y = px + 0.5, py + 0.5
            a = rounded_rect_alpha(x, y, n, n, radius)
            if a <= 0.0:
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
                    colour = TRACK
                else:
                    for end_y in (track_top, track_bottom):
                        if math.hypot(x - cx, y - end_y) <= track_w / 2:
                            colour = TRACK
                # Knob.
                if math.hypot(x - cx, y - n * fy) <= knob_r:
                    colour = accent
            row.append(colour + (255,))
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


def write_png(path, size):
    raw = render(size)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)
    print(f"wrote {path} ({size}x{size}, {len(png)} bytes)")


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    os.makedirs(out_dir, exist_ok=True)
    for size in (256, 128, 64, 48, 32):
        write_png(os.path.join(out_dir, f"linuxsonar-{size}.png"), size)
    # The canonical icon the desktop entry and AppImage refer to.
    write_png(os.path.join(out_dir, "linuxsonar.png"), 256)


if __name__ == "__main__":
    main()
