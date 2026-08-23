#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Generate the bass-synth wavetables: dual-layer per engine.
#
# For each engine we produce 3 single-cycle 256-sample mono wavs:
#
#   <engine>_body_bright.wav   — full body of the timbre, bright variant
#   <engine>_body_dark.wav     — full body, lowpass-approximated (darker)
#   <engine>_sub.wav           — sub layer (always present, low gain)
#
# The ROM uses two mixer channels per note: one playing body_bright or
# body_dark (crossfaded live by stick X), one playing sub at fixed gain.
# That gives every note a real "weight" — the sub layer is the fundamental
# the speakers reproduce, the body layer is what gives the engine identity.

import math
import os
import struct
import sys

SR = 32000
N = 256
AMP = 0.85

def write_wav(path, samples):
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + 2 * len(samples)))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 1, SR, SR * 2, 2, 16))
        f.write(b"data")
        f.write(struct.pack("<I", 2 * len(samples)))
        for s in samples:
            s = max(-1.0, min(1.0, s))
            f.write(struct.pack("<h", int(s * 32767)))

def saw(i):
    return ((i / N) * 2.0 - 1.0)

def sine(i, phase=0.0):
    return math.sin(2.0 * math.pi * (i / N) + phase)

def square(i, duty=0.5):
    return 1.0 if (i / N) < duty else -1.0

def noise_prng(seed):
    x = seed
    while True:
        x = (1103515245 * x + 12345) & 0x7fffffff
        yield (x / 0x7fffffff) * 2.0 - 1.0

def clip(x, drive):
    return math.tanh(x * drive) / math.tanh(drive) if drive > 1.0 else x

def avg_filter(samples, k=3):
    out = []
    for i in range(len(samples)):
        s = 0.0
        for j in range(-k, k + 1):
            s += samples[(i + j) % len(samples)]
        out.append(s / (2 * k + 1))
    return out

# ── HEAVY: aggressive saw + low sub ───────────────────────────────────────
def heavy_body_bright():
    return [saw(i) * AMP for i in range(N)]

def heavy_body_dark():
    return avg_filter([saw(i) for i in range(N)], k=4)

def heavy_sub():
    # Pure low sine, no harmonics — sub layer is the fundamental.
    return [sine(i) * AMP * 0.9 for i in range(N)]

# ── SUB: pure 808-style sine + transient body ─────────────────────────────
def sub_body_bright():
    # Body = sine + 3rd harmonic for "presence" (808 has subtle harmonics)
    return [(sine(i) + 0.18 * sine(i, 2 * math.pi * 3 * i / N - math.pi / 4)) * AMP * 0.85
            for i in range(N)]

def sub_body_dark():
    return [sine(i) * AMP * 0.85 for i in range(N)]

def sub_sub():
    # Body + low sine sub layer (1 octave down via half-rate playback by the
    # mixer; the mixer pitches the sub channel at half the body's freq).
    # Here we just emit a sine — same as the body, the layering is by mix.
    return [sine(i) * AMP * 0.9 for i in range(N)]

# ── GROWL: 2-op FM, modulated carrier ─────────────────────────────────────
def growl_fm(idx, ratio):
    out = []
    for i in range(N):
        t = i / N
        mod = math.sin(2 * math.pi * ratio * t)
        car = math.sin(2 * math.pi * t + idx * mod)
        out.append(car * AMP)
    return out

def growl_body_bright():
    return growl_fm(idx=2.0, ratio=2.0)

def growl_body_dark():
    return growl_fm(idx=1.0, ratio=1.0)

def growl_sub():
    # Low sine — for FM voices the sub is what makes it *feel* heavy.
    return [sine(i) * AMP * 0.9 for i in range(N)]

# ── INDUSTRIAL: square + noise ────────────────────────────────────────────
def industrial_body_bright():
    gen = noise_prng(0xDEAD)
    return [(square(i) * 0.6 + next(gen) * 0.4) * AMP for i in range(N)]

def industrial_body_dark():
    return avg_filter([square(i) for i in range(N)], k=5)

def industrial_sub():
    # Industrial sub = pure square at the fundamental — gives it teeth.
    return [square(i) * AMP * 0.85 for i in range(N)]

ENGINES = ["heavy", "sub", "growl", "industrial"]

def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "assets/bass_wav"
    os.makedirs(out_dir, exist_ok=True)

    writers = {
        "heavy":       (heavy_body_bright, heavy_body_dark, heavy_sub),
        "sub":         (sub_body_bright,   sub_body_dark,   sub_sub),
        "growl":       (growl_body_bright, growl_body_dark, growl_sub),
        "industrial":  (industrial_body_bright, industrial_body_dark, industrial_sub),
    }

    for e in ENGINES:
        bb, bd, sub = writers[e]
        for name, fn in [("body_bright", bb), ("body_dark", bd), ("sub", sub)]:
            path = os.path.join(out_dir, f"{e}_{name}.wav")
            write_wav(path, fn())
            print(f"wrote {path}")

if __name__ == "__main__":
    main()