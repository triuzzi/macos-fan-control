#!/usr/bin/env python3
import math
import struct
import zlib
from pathlib import Path

SIZE = 512
SAMPLES = 4
CORNER = 0.22
BLADES = 4
INNER = 0.17
OUTER = 0.82
SWEEP = 1.15
WIDTH = 0.62
HUB = 0.13
BACKGROUND = (0x1C, 0x6E, 0xD8)
BLADE = (0xFF, 0xFF, 0xFF)


def inside_tile(x, y):
    ax, ay = abs(x), abs(y)
    straight = 1.0 - CORNER
    if ax <= straight or ay <= straight:
        return ax <= 1.0 and ay <= 1.0
    return (ax - straight) ** 2 + (ay - straight) ** 2 <= CORNER**2


def inside_fan(x, y):
    radius = math.hypot(x, y)
    if radius <= HUB:
        return True
    if radius < INNER or radius > OUTER:
        return False
    progress = (radius - INNER) / (OUTER - INNER)
    half = WIDTH * math.sin(math.pi * progress) ** 0.65
    angle = math.atan2(y, x)
    for blade in range(BLADES):
        swept = angle - (2 * math.pi / BLADES) * blade - SWEEP * progress
        if abs((swept + math.pi) % (2 * math.pi) - math.pi) <= half:
            return True
    return False


def rows():
    offsets = [(index + 0.5) / SAMPLES for index in range(SAMPLES)]
    total = SAMPLES * SAMPLES
    for pixel_y in range(SIZE):
        row = bytearray()
        for pixel_x in range(SIZE):
            tile = blade = 0
            for offset_y in offsets:
                y = ((pixel_y + offset_y) / SIZE) * 2 - 1
                for offset_x in offsets:
                    x = ((pixel_x + offset_x) / SIZE) * 2 - 1
                    if not inside_tile(x, y):
                        continue
                    tile += 1
                    if inside_fan(x, y):
                        blade += 1
            if not tile:
                row += bytes(4)
                continue
            mix = blade / tile
            row += bytes(
                [round(BACKGROUND[channel] * (1 - mix) + BLADE[channel] * mix) for channel in range(3)]
                + [round(255 * tile / total)]
            )
        yield bytes(row)


def chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)


target = Path(__file__).resolve().parent.parent / "interfaces" / "raycast" / "assets" / "extension-icon.png"
scanlines = b"".join(b"\x00" + row for row in rows())
target.write_bytes(
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(scanlines, 9))
    + chunk(b"IEND", b"")
)
print(f"wrote {target}")
