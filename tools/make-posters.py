#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate placeholder 16:9 thumbnails (stdlib only): a colour gradient with a large number.

    tools/make-posters.py OUT_DIR [COUNT] [WIDTH] [HEIGHT]

Writes OUT_DIR/poster-01.png ... Used by the example app; replace with real artwork in a product.
"""
import colorsys
import struct
import sys
import zlib
from pathlib import Path

DIGITS = {  # 5x7 bitmap font
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
}


def png(w, h, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def poster(i, n, w, h):
    hue = i / n
    c1 = colorsys.hsv_to_rgb(hue, 0.55, 0.55)
    c2 = colorsys.hsv_to_rgb((hue + 0.12) % 1, 0.75, 0.25)
    text = f"{i + 1:02d}"
    scale = h // 12
    tw, th = (len(text) * 6 - 1) * scale, 7 * scale
    ox, oy = (w - tw) // 2, (h - th) // 2 - h // 12
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            t = (x / w + y / h) / 2
            r, g, b = (c1[k] * (1 - t) + c2[k] * t for k in range(3))
            gx, gy = x - ox, y - oy
            if 0 <= gx < tw and 0 <= gy < th:
                ch, cx = divmod(gx // scale, 6)
                if cx < 5 and DIGITS[text[ch]][gy // scale][cx] == "1":
                    r = g = b = 0.95
            row += bytes(int(v * 255) for v in (r, g, b))
        rows.append(bytes(row))
    return png(w, h, rows)


def main():
    out = Path(sys.argv[1])
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 480
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 270
    out.mkdir(parents=True, exist_ok=True)
    for i in range(n):
        (out / f"poster-{i + 1:02d}.png").write_bytes(poster(i, n, w, h))
    print(f"make-posters: wrote {n} images {w}x{h} to {out}")


if __name__ == "__main__":
    main()
