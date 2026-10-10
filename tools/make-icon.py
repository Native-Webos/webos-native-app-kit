#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a simple placeholder app icon as PNG (stdlib only).

    tools/make-icon.py OUT.png SIZE

Draws white artwork (a rounded "play" triangle inside a ring) on a transparent background; webOS
paints appinfo.json `iconColor` behind it. Replace with real artwork for a published app.
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
import zlib
from pathlib import Path


def png(width: int, height: int, rgba: bytes) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    rows = b"".join(b"\x00" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9))
            + chunk(b"IEND", b""))


def render(size: int) -> bytes:
    out = bytearray(size * size * 4)
    c = (size - 1) / 2.0
    r_out, r_in = size * 0.42, size * 0.35
    # Triangle pointing right, centred.
    tri = [(c - size * 0.12, c - size * 0.17), (c - size * 0.12, c + size * 0.17), (c + size * 0.18, c)]

    def inside_tri(x: float, y: float) -> bool:
        (x1, y1), (x2, y2), (x3, y3) = tri
        d1 = (x - x2) * (y1 - y2) - (x1 - x2) * (y - y2)
        d2 = (x - x3) * (y2 - y3) - (x2 - x3) * (y - y3)
        d3 = (x - x1) * (y3 - y1) - (x3 - x1) * (y - y1)
        neg = d1 < 0 or d2 < 0 or d3 < 0
        pos = d1 > 0 or d2 > 0 or d3 > 0
        return not (neg and pos)

    for y in range(size):
        for x in range(size):
            # 4x4 supersampling for smooth edges.
            hits = 0
            for sy in range(4):
                for sx in range(4):
                    px, py = x + (sx + 0.5) / 4, y + (sy + 0.5) / 4
                    d = math.hypot(px - c - 0.5, py - c - 0.5)
                    if r_in <= d <= r_out or inside_tri(px - 0.5, py - 0.5):
                        hits += 1
            a = hits * 255 // 16
            i = (y * size + x) * 4
            out[i:i + 4] = bytes((255, 255, 255, a))
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", type=Path)
    ap.add_argument("size", type=int)
    args = ap.parse_args()
    if not 16 <= args.size <= 1024:
        print("make-icon: size must be 16..1024", file=sys.stderr)
        return 2
    args.out.write_bytes(png(args.size, args.size, render(args.size)))
    print(f"make-icon: wrote {args.out} ({args.size}x{args.size})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
