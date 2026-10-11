#!/usr/bin/env python3
"""Writes the sample effect's textures (Content/Effects/Texture): soft white shapes in the alpha channel.

Usage: python3 Games/Sample/Tools/GenerateEffectTextures.py [output folder]
"""
import math, struct, sys, zlib, os
def png(path, w, h, alpha):
    rows = b''.join(b'\x00' + b''.join(struct.pack('4B', 255, 255, 255, max(0, min(255, int(alpha(x, y) * 255 + 0.5)))) for x in range(w)) for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'Content', 'Effects', 'Texture')
N = 128
def r(x, y): return math.hypot((x + 0.5) / N * 2 - 1, (y + 0.5) / N * 2 - 1)
# A soft round glow.
png(os.path.join(out, 'Particle01.png'), N, N, lambda x, y: max(0.0, 1 - r(x, y)) ** 2)
# A bright core with a thin halo: sparks.
png(os.path.join(out, 'Particle02.png'), N, N, lambda x, y: min(1.0, math.exp(-(r(x, y) / 0.18) ** 2) + 0.35 * max(0.0, 1 - r(x, y)) ** 3))
# A beam: bright along the middle row, fading up and down, softened at the ends.
def beam(x, y):
    v = abs((y + 0.5) / N * 2 - 1)
    u = abs((x + 0.5) / N * 2 - 1)
    return math.exp(-(v / 0.22) ** 2) * min(1.0, (1 - u) * 6)
png(os.path.join(out, 'LaserMain01.png'), N, N, beam)
