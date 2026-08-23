#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# examples/music/synth_loop.py — synthesise a 15-second dark-sci-fi loop
# to a 32 kHz mono 16-bit WAV that audioconv64 turns into a VADPCM .wav64
# for the cinematic-demo. No MIDI, no soundfont, no .xm — just waveforms
# and noise, the same way a chiptune does it, but at 32 kHz so the cutoff
# frequencies we're using actually fit.
#
# Layer stack (one bar of 4/4 at 60 BPM = 4 s, then a 4-bar phrase = 16 s
#   with the last 1 s tailed; we trim to 15 s for loop cleanliness):
#   1. Sub-bass drone    — sine 50 Hz, mild vibrato, sustained
#   2. Mid drone         — saw 110 Hz, slow LFO tremolo, sustained
#   3. Percussion hit    — bandpass-filtered noise burst, every beat
#   4. Hi-hat            — highpass noise click, every 1/8 note
#   5. Sweep swell       — pitch-bent sine 80 → 40 Hz, every 4 bars
#
# The point is a moody underscore, not a song. Doesn't fight the dialogue
# or footsteps — ~ 2 octaves below the goblin's voice would land, with
# rhythmic motion at the kick rate.

import math
import struct
import wave
import os

SR = 32000
DURATION = 15.0           # seconds
BPM = 60.0                # one beat per second — slow, breathable
BEAT = SR / BPM           # samples per beat
BAR = 4 * BEAT            # 4/4

# master volume — keep peaks below 0.5 so audioconv64's VADPCM has headroom
MASTER = 0.45

N = int(SR * DURATION)
out = [0.0] * N


def add_layer(samples, fn):
    """fn(t_seconds) -> float in [-1, 1]; add into `samples`."""
    for i in range(len(samples)):
        samples[i] += fn(i / SR)


# 1. Sub-bass drone — 50 Hz sine with slight vibrato
def sub_bass(i):
    t = i / SR
    vib = math.sin(2 * math.pi * 0.12 * t) * 1.5     # ±1.5 Hz vibrato
    f = 50.0 + vib
    s = math.sin(2 * math.pi * f * t)
    # Slow fade in/out so loop boundary is clean
    env = min(1.0, t / 0.5) * min(1.0, (DURATION - t) / 0.5)
    return 0.30 * s * env


# 2. Mid drone — saw 110 Hz with tremolo
def mid_drone(i):
    t = i / SR
    # 8 harmonics of a 110 Hz saw, soft-clipped to keep it dark
    f0 = 110.0
    s = 0.0
    for k in range(1, 9):
        s += math.sin(2 * math.pi * k * f0 * t) / k
    s *= 0.6                                          # rough normalisation
    tremolo = 0.6 + 0.4 * math.sin(2 * math.pi * 0.25 * t)
    env = min(1.0, t / 1.0) * min(1.0, (DURATION - t) / 1.0)
    return 0.18 * s * tremolo * env


# 3. Percussion hit — bandpass-ish noise burst on every beat
import random
random.seed(0xDEADBEEF)
def percussion(i):
    t = i / SR
    beat_n = int(t * BPM)
    phase = (t - beat_n / BPM) * BPM                  # 0..1 within beat
    if phase > 0.18:                                  # hit length
        return 0.0
    decay = math.exp(-phase * 28.0)
    # Crude bandpass: subtract lowpass from broadband
    n = random.uniform(-1, 1)
    # 1-pole lowpass at 800 Hz
    prev_lp = percussion._lp_prev if hasattr(percussion, '_lp_prev') else 0.0
    a = 0.10
    lp = prev_lp + a * (n - prev_lp)
    percussion._lp_prev = lp
    return 0.50 * (n - lp) * decay


# 4. Hi-hat — highpass noise click every 1/8 note
def hihat(i):
    t = i / SR
    eighth = t * BPM * 2                              # 8th-notes per second
    n_int = int(eighth)
    phase = (eighth - n_int)
    if phase > 0.05:
        return 0.0
    decay = math.exp(-phase * 90.0)
    n = random.uniform(-1, 1)
    # skip a 1-pole lowpass by going through two LPs for "high" character
    return 0.20 * n * decay


# 5. Sweep swell — pitch-bent sine glides, every bar
def sweep(i):
    t = i / SR
    bar_n = int(t * BPM / 4)
    phase = (t - bar_n * 4 / BPM)
    if phase > 3.5:
        return 0.0
    # Start at 80 Hz, glide down to 40, rise to 60
    f = 80.0 + 20.0 * math.sin(2 * math.pi * 0.5 * phase)
    s = math.sin(2 * math.pi * f * phase)
    env = math.sin(math.pi * phase / 3.5)            # bell envelope
    return 0.10 * s * env


# Composite
add_layer(out, sub_bass)
add_layer(out, mid_drone)
add_layer(out, percussion)
add_layer(out, hihat)
add_layer(out, sweep)

# Mix down to 16-bit with master gain and a touch of soft saturation
def soft_clip(x):
    return math.tanh(x * 1.4) / 1.4

peak = max(abs(x) for x in out)
if peak > 0.0:
    # Normalise to 0.95 peak
    norm = 0.95 / peak * MASTER
else:
    norm = 1.0

frames = bytearray()
for s in out:
    v = int(soft_clip(s * norm) * 32767)
    v = max(-32768, min(32767, v))
    frames += struct.pack('<h', v)

# Write WAV
out_path = os.path.join(os.path.dirname(__file__), 'cine_loop.wav')
with wave.open(out_path, 'wb') as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(bytes(frames))
print(f"wrote {out_path}: {len(frames)} bytes, peak={peak:.3f}, post-norm peak={0.95*MASTER:.3f}")
