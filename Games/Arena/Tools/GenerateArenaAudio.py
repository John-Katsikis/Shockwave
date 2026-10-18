"""Regenerate the tiny original CC0 arena sounds; Python standard library only."""
import math
import random
import struct
import wave
from pathlib import Path

folder = Path(__file__).resolve().parents[1] / "Content" / "Audio"
for name, duration in [("shot", 0.14), ("reload", 0.35), ("hit", 0.12)]:
    rng = random.Random(42)
    samples = []
    for i in range(int(24000 * duration)):
        t = i / 24000
        if name == "shot":
            value = (rng.uniform(-1, 1) * 0.55 + math.sin(t * 2 * math.pi * 95) * 0.45) * math.exp(-t * 38)
        elif name == "reload":
            value = rng.uniform(-1, 1) * (math.exp(-t * 100) + (math.exp(-(t - 0.23) * 90) if t >= 0.23 else 0))
        else:
            value = math.sin(t * 2 * math.pi * 1050) * math.exp(-t * 35) * 0.5
        samples.append(struct.pack("<h", int(max(-1, min(1, value)) * 26000)))
    with wave.open(str(folder / f"arena-{name}.wav"), "wb") as out:
        out.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
        out.writeframes(b"".join(samples))
