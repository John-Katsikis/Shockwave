"""Regenerate Reactor Run's small original assets (CC0); Python standard library only.

Writes every model, sound and UI image under Content/ the demo uses, byte for byte the
same each run, so the files in the repository can be checked against this script:

    python3 Games/ShippingDemo/Tools/GenerateDemoAssets.py

Nothing is downloaded and no third-party sample is used. Content/PROVENANCE.md lists
each file and what it's for.
"""
import json
import math
import random
import struct
import wave
import zlib
from pathlib import Path

content = Path(__file__).resolve().parents[1] / "Content"


# ---- Sounds ------------------------------------------------------------------------------

def write_wav(name, duration, voice, rate=24000):
    rng = random.Random(7)
    frames = []
    for i in range(int(rate * duration)):
        t = i / rate
        v = max(-1.0, min(1.0, voice(t, rng)))
        frames.append(struct.pack("<h", int(v * 26000)))
    with wave.open(str(content / "Audio" / name), "wb") as out:
        out.setparams((1, 2, rate, 0, "NONE", "not compressed"))
        out.writeframes(b"".join(frames))


def sounds():
    tau = 2 * math.pi
    write_wav("shot.wav", 0.16, lambda t, r: (r.uniform(-1, 1) * 0.6 + math.sin(t * tau * 110) * 0.4) * math.exp(-t * 34))
    write_wav("reload.wav", 0.4, lambda t, r: r.uniform(-1, 1) * (math.exp(-t * 90) + (math.exp(-(t - 0.26) * 80) if t >= 0.26 else 0)) * 0.7)
    write_wav("hit.wav", 0.12, lambda t, r: math.sin(t * tau * 1200) * math.exp(-t * 32) * 0.5)
    write_wav("pickup.wav", 0.3, lambda t, r: math.sin(t * tau * (660 + 440 * t / 0.3)) * math.exp(-t * 9) * 0.5)
    write_wav("hurt.wav", 0.2, lambda t, r: (math.sin(t * tau * 180) * 0.6 + r.uniform(-1, 1) * 0.3) * math.exp(-t * 18))
    # Level-specific: only training uses the chime, only the factory the alarm.
    write_wav("training-chime.wav", 1.2, lambda t, r: (math.sin(t * tau * 523.25) + 0.5 * math.sin(t * tau * 784)) * math.exp(-t * 3) * 0.35)
    write_wav("factory-alarm.wav", 2.0, lambda t, r: math.sin(t * tau * (440 if (t * 2) % 1 < 0.5 else 330)) * 0.18)


# ---- Models (binary glTF) ------------------------------------------------------------

def prism(sides, radius, depth, color, emissive=(0, 0, 0)):
    """A flat prism facing +Z/-Z (a target plate), as one mesh with flat normals."""
    pos, nrm, idx = [], [], []

    def add_face(points, normal):
        base = len(pos)
        for p in points:
            pos.append(p)
            nrm.append(normal)
        for i in range(1, len(points) - 1):
            idx.extend([base, base + i, base + i + 1])

    ring = [(radius * math.cos(a), radius * math.sin(a)) for a in (2 * math.pi * k / sides for k in range(sides))]
    h = depth / 2
    add_face([(x, y, h) for x, y in ring], (0, 0, 1))
    add_face([(x, y, -h) for x, y in reversed(ring)], (0, 0, -1))
    for k in range(sides):
        (x0, y0), (x1, y1) = ring[k], ring[(k + 1) % sides]
        nx, ny = (x0 + x1) / 2, (y0 + y1) / 2
        length = math.hypot(nx, ny)
        add_face([(x0, y0, -h), (x1, y1, -h), (x1, y1, h), (x0, y0, h)], (nx / length, ny / length, 0))
    return pos, nrm, idx, color, emissive


def box(half, color, emissive=(0, 0, 0)):
    hx, hy, hz = half
    pos, nrm, idx = [], [], []
    faces = [
        ((1, 0, 0), [(hx, -hy, hz), (hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz)]),
        ((-1, 0, 0), [(-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)]),
        ((0, 1, 0), [(-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (-hx, hy, -hz)]),
        ((0, -1, 0), [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)]),
        ((0, 0, 1), [(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]),
        ((0, 0, -1), [(hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz)]),
    ]
    for normal, points in faces:
        base = len(pos)
        for p in points:
            pos.append(p)
            nrm.append(normal)
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    return pos, nrm, idx, color, emissive


def write_glb(name, parts):
    """parts: (positions, normals, indices, base color, emissive) per primitive."""
    blob = bytearray()
    views, accessors, materials, primitives = [], [], [], []

    def view(data, target):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), "target": target})
        blob.extend(data)
        return len(views) - 1

    for pos, nrm, idx, color, emissive in parts:
        p = view(b"".join(struct.pack("<3f", *v) for v in pos), 34962)
        lo = [min(v[i] for v in pos) for i in range(3)]
        hi = [max(v[i] for v in pos) for i in range(3)]
        accessors.append({"bufferView": p, "componentType": 5126, "count": len(pos), "type": "VEC3", "min": lo, "max": hi})
        n = view(b"".join(struct.pack("<3f", *v) for v in nrm), 34962)
        accessors.append({"bufferView": n, "componentType": 5126, "count": len(nrm), "type": "VEC3"})
        i = view(b"".join(struct.pack("<H", v) for v in idx), 34963)
        accessors.append({"bufferView": i, "componentType": 5123, "count": len(idx), "type": "SCALAR"})
        materials.append({
            "pbrMetallicRoughness": {"baseColorFactor": list(color) + [1.0], "metallicFactor": 0.1, "roughnessFactor": 0.7},
            "emissiveFactor": list(emissive),
        })
        primitives.append({"attributes": {"POSITION": len(accessors) - 3, "NORMAL": len(accessors) - 2},
                           "indices": len(accessors) - 1, "material": len(materials) - 1})
    while len(blob) % 4:
        blob.append(0)
    doc = {
        "asset": {"version": "2.0", "generator": "GenerateDemoAssets.py (CC0)"},
        "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": Path(name).stem}],
        "meshes": [{"primitives": primitives}],
        "materials": materials, "accessors": accessors, "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
    }
    text = json.dumps(doc, separators=(",", ":"), sort_keys=True).encode()
    while len(text) % 4:
        text += b" "
    out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(blob))
    out += struct.pack("<II", len(text), 0x4E4F534A) + text
    out += struct.pack("<II", len(blob), 0x004E4942) + bytes(blob)
    (content / "Models" / name).write_bytes(out)


def models():
    # Training's target: a red plate with a white ring, 0.8 m across.
    write_glb("training-target.glb", [
        prism(16, 0.4, 0.08, (0.82, 0.16, 0.12)),
        prism(16, 0.22, 0.1, (0.95, 0.95, 0.92)),
        prism(16, 0.09, 0.12, (0.82, 0.16, 0.12), (0.6, 0.05, 0.02)),
    ])
    # The factory's crate: a dark box with an amber hazard band.
    write_glb("factory-crate.glb", [
        box((0.5, 0.5, 0.5), (0.22, 0.24, 0.25)),
        box((0.51, 0.08, 0.51), (0.95, 0.62, 0.1), (0.25, 0.15, 0.0)),
    ])


# ---- UI images (PNG) -------------------------------------------------------------------

def write_png(name, size, pixel):
    rows = b""
    for y in range(size):
        rows += b"\x00" + b"".join(bytes(pixel(x / (size - 1), y / (size - 1))) for x in range(size))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    (content / "UI" / name).write_bytes(png)


def images():
    def badge(color):
        def pixel(u, v):
            d = math.hypot(u - 0.5, v - 0.5)
            if d > 0.48:
                return (0, 0, 0, 0)
            if d > 0.4:
                return (237, 244, 245, 255)
            return (*color, 255)
        return pixel

    write_png("training-badge.png", 64, badge((80, 208, 239)))
    write_png("factory-badge.png", 64, badge((255, 163, 91)))


if __name__ == "__main__":
    sounds()
    models()
    images()
    print("Wrote", content)
