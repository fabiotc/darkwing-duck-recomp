"""Draws the app icon (original artwork: night sky, moon and skyline).

    python3 packaging/make_icon.py packaging/icon.png
"""

import math
import struct
import sys
import zlib

N = 512


def png(path: str, px: list[list[tuple[int, int, int, int]]]) -> None:
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in px)

    def chunk(t: bytes, b: bytes) -> bytes:
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def inside_rounded(x: float, y: float, r: float = 96) -> float:
    """Coverage (0..1) of a rounded square, for soft edges."""
    m = 16  # margin
    cx = min(max(x, m + r), N - m - r)
    cy = min(max(y, m + r), N - m - r)
    d = math.hypot(x - cx, y - cy)
    return max(0.0, min(1.0, r - d + 0.5))


# Skyline: (left, right, top) in pixels
BUILDINGS = [(40, 100, 380), (95, 150, 330), (145, 215, 400), (210, 250, 300), (245, 300, 360),
             (295, 360, 320), (355, 400, 390), (395, 470, 345)]
WINDOWS = [(110, 340), (125, 360), (160, 410), (225, 320), (265, 380), (310, 335), (330, 360), (410, 360),
           (430, 380), (60, 395)]


def pixel(x: int, y: int) -> tuple[int, int, int, int]:
    a = inside_rounded(x + 0.5, y + 0.5)
    if a <= 0:
        return (0, 0, 0, 0)
    t = y / N
    r, g, b = int(40 + 50 * t), int(18 + 20 * t), int(90 + 40 * t)  # purple night gradient
    # stars
    h = (x * 73856093 ^ y * 19349663) & 0xFFFF
    if h < 6 and y < 300:
        r, g, b = 230, 230, 255
    # moon with a soft glow
    mx, my, mr = 330, 175, 105
    d = math.hypot(x - mx, y - my)
    if d < mr + 40:
        glow = max(0.0, 1 - (d - mr) / 40) * 0.35 if d > mr else 0
        r, g, b = int(r + (255 - r) * glow), int(g + (210 - g) * glow), int(b + (120 - b) * glow)
    if d < mr:
        r, g, b = 250, 196, 70
        for cx, cy, cr in ((300, 150, 22), (360, 210, 16), (345, 130, 10), (290, 215, 12)):
            if math.hypot(x - cx, y - cy) < cr:
                r, g, b = 222, 160, 52
    # skyline
    for left, right, top in BUILDINGS:
        if left <= x < right and y >= top:
            r, g, b = 16, 10, 30
            for wx, wy in WINDOWS:
                if wx <= x < wx + 10 and wy <= y < wy + 14 and left <= wx < right:
                    r, g, b = 255, 214, 90
    if y >= 470:
        r, g, b = 16, 10, 30
    return (r, g, b, int(255 * a))


if __name__ == "__main__":
    png(sys.argv[1], [[pixel(x, y) for x in range(N)] for y in range(N)])
