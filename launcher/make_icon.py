"""make_icon.py OUT.ico: the launcher's icon -- an orange-rimmed navy hexagon with a
cyan inner hexagon (A Crack in Time's HUD shapes), drawn here (no game art), as a
PNG-in-ICO with 16, 24, 32, 48, 64 and 256 px images. Pure Python."""
import math, struct, sys, zlib


def hexagon(cx, cy, r):
    return [(cx + r * math.cos(math.radians(60 * i - 90)), cy + r * math.sin(math.radians(60 * i - 90))) for i in range(6)]


def inside(poly, x, y):
    n, c = len(poly), False
    for i in range(n):
        x1, y1 = poly[i]; x2, y2 = poly[(i + 1) % n]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            c = not c
    return c


def render(size):
    ss = 4
    s = size / 256.0
    layers = [  # (polygon, rgba) back to front
        (hexagon(128 * s, 128 * s, 124 * s), (255, 155, 33, 255)),   # orange rim
        (hexagon(128 * s, 128 * s, 110 * s), (12, 28, 50, 255)),     # navy body
        (hexagon(128 * s, 128 * s, 78 * s), (76, 195, 245, 255)),    # cyan ring
        (hexagon(128 * s, 128 * s, 64 * s), (16, 39, 66, 255)),      # navy core
        (hexagon(128 * s, 128 * s, 30 * s), (255, 155, 33, 255)),    # orange centre
    ]
    px = bytearray()
    for y in range(size):
        px.append(0)  # PNG filter: none
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    fx, fy = x + (sx + 0.5) / ss, y + (sy + 0.5) / ss
                    c = None
                    for poly, rgba in layers:
                        if inside(poly, fx, fy):
                            c = rgba
                    if c:
                        a = c[3] / 255.0
                        acc[0] += c[0] * a; acc[1] += c[1] * a; acc[2] += c[2] * a; acc[3] += a
            n = ss * ss
            a = acc[3] / n
            if a > 0:
                px += bytes([int(acc[0] / acc[3]), int(acc[1] / acc[3]), int(acc[2] / acc[3]), int(a * 255 + 0.5)])
            else:
                px += b'\0\0\0\0'

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(px), 9)) + chunk(b'IEND', b''))


sizes = [16, 24, 32, 48, 64, 256]
pngs = [render(z) for z in sizes]
out = struct.pack('<HHH', 0, 1, len(sizes))
off = 6 + 16 * len(sizes)
for z, p in zip(sizes, pngs):
    out += struct.pack('<BBBBHHII', z % 256, z % 256, 0, 0, 1, 32, len(p), off)
    off += len(p)
open(sys.argv[1], 'wb').write(out + b''.join(pngs))
print('wrote', sys.argv[1])
