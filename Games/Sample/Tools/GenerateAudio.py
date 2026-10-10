#!/usr/bin/env python3
"""Generate the sample audio from math. Original work dedicated to CC0 1.0."""
from pathlib import Path
import math
import struct
import wave

ROOT = Path(__file__).resolve().parents[1] / "Content" / "Audio"
ROOT.mkdir(parents=True, exist_ok=True)
RATE = 48000

def write(name, seconds, signal):
    count = round(seconds * RATE)
    pcm = bytearray()
    for frame in range(count):
        t = frame / RATE
        value = max(-1, min(1, signal(t, frame, count)))
        pcm.extend(struct.pack("<h", round(value * 32767)))
    with wave.open(str(ROOT / name), "wb") as clip:
        clip.setnchannels(1)
        clip.setsampwidth(2)
        clip.setframerate(RATE)
        clip.writeframes(pcm)

# Integral periods make the loop seamless, including its slow amplitude modulation.
write("ambient.wav", 2, lambda t, *_: .12 * math.sin(2 * math.pi * 110 * t) * (.7 + .3 * math.cos(2 * math.pi * .5 * t)))
write("pulse.wav", .18, lambda t, frame, count: .3 * math.sin(2 * math.pi * 660 * t) * min(1, t / .005) * max(0, 1 - frame / count) ** 2)
