#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# analyse.py — measure a rendered instrument, because nobody can listen to
# thirty-odd instruments in a Nix sandbox.
#
# usage:
#   analyse.py <file.wav> --kind harmonic|inharmonic|noise --freq HZ
#                         [--tail decay|sustain] [--gate-off SECONDS] [--alias 1.0,0.5] [--json]
#   analyse.py --ratio <lo.wav> <hi.wav> --octave-up N     # does `freq` move the spectrum?
#
# What it measures, and what each number is for:
#
#   peak / rms     level. mkBakedInstrument already refuses silence, < -40 dBFS
#                  and clipping; they are reported here so a report reads whole.
#   dc             |mean| / peak. A decaying DC step is what dsp/ks.dsp once
#                  rendered while every loudness gate passed.
#   f0 / corr      autocorrelation pitch of the attack-to-sustain window, and
#                  how periodic it is. Harmonic instruments must land within
#                  3% of the requested frequency.
#   centroid       spectral centroid (Hz) of the same window. Inharmonic
#                  instruments (bells, plates, drums) have no f0 to measure; what
#                  the `freq` parameter must still do is move the whole
#                  spectrum, so the OCTAVE check compares the centroid at f and
#                  2f and wants a ratio near 2.
#   hf             fraction of energy above 0.40*SR. A band-limited oscillator
#                  keeps this small; a naive saw does not. It is the aliasing
#                  canary — aliased partials fold into exactly this region.
#   tail           how far the last 15% of the file is below its loudest 50 ms.
#                  `decay` instruments must have rung out; `sustain` ones are
#                  checked at the release instead (gate-off).
#
# Pure Python on purpose, like ks_pitch.py: a numpy fetch is not worth it for
# 8192-point transforms.
import cmath
import json
import math
import struct
import sys
import wave


def load(path):
    w = wave.open(path)
    sr, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
    raw = w.readframes(w.getnframes())
    if sw != 2:
        sys.exit("analyse: %s is %d-byte PCM, expected 16-bit" % (path, sw))
    s = struct.unpack("<%dh" % (len(raw) // 2), raw)
    # Mix to mono for measurement; the instruments are mono or Haas-widened.
    if ch == 1:
        return sr, [v / 32768.0 for v in s]
    return sr, [sum(s[i:i + ch]) / (32768.0 * ch) for i in range(0, len(s), ch)]


def fft(x):
    n = len(x)
    if n == 1:
        return x
    a = fft(x[0::2])
    b = fft(x[1::2])
    out = [0] * n
    for k in range(n // 2):
        t = cmath.exp(-2j * math.pi * k / n) * b[k]
        out[k] = a[k] + t
        out[k + n // 2] = a[k] - t
    return out


def spectrum(x, n=8192):
    """Magnitude-squared of a Hann-windowed n-point block, bins 0..n/2."""
    x = (x + [0.0] * n)[:n]
    win = [0.5 - 0.5 * math.cos(2 * math.pi * i / n) for i in range(n)]
    f = fft([v * w for v, w in zip(x, win)])
    return [abs(c) ** 2 for c in f[: n // 2]]


def rms(seg):
    return math.sqrt(sum(v * v for v in seg) / max(1, len(seg)))


def db(v):
    return 20 * math.log10(max(v, 1e-9))


def onset(x, thresh=0.1):
    peak = max(abs(v) for v in x)
    for i, v in enumerate(x):
        if abs(v) >= thresh * peak:
            return i
    return 0


def window(x, sr, start, length=0.25):
    """The analysis window: `length` seconds starting `start` seconds after onset."""
    i0 = onset(x) + int(start * sr)
    return x[i0: i0 + int(length * sr)]


def centroid(seg, sr):
    n = 8192
    p = spectrum(seg, n)
    tot = sum(p) or 1.0
    return sum(k * (sr / n) * v for k, v in enumerate(p)) / tot


def hf_fraction(seg, sr, frac=0.40):
    n = 8192
    p = spectrum(seg, n)
    tot = sum(p) or 1.0
    cut = int(frac * sr / (sr / n))
    return sum(p[cut:]) / tot


def autocorr_f0(seg, sr, lo_hz=40.0, hi_hz=4000.0):
    mean = sum(seg) / len(seg)
    seg = [v - mean for v in seg]
    energy = sum(v * v for v in seg) or 1.0
    lo, hi = int(sr / hi_hz), min(int(sr / lo_hz), len(seg) // 2)
    corr = []
    for lag in range(lo, hi):
        corr.append(sum(seg[i] * seg[i + lag] for i in range(len(seg) - lag)) / energy)
    first_neg = next((k for k, c in enumerate(corr) if c < 0), None)
    if first_neg is None:
        return 0.0, 0.0
    # Parabolic refinement around the strongest lag after the main lobe.
    k = max(range(first_neg, len(corr)), key=lambda j: corr[j])
    best = corr[k]
    # Prefer the SHORTEST lag that is nearly as good: an octave-down lag of a
    # strong harmonic wave correlates almost as well and must not win.
    for j in range(first_neg, k):
        if corr[j] > 0.92 * best and corr[j] >= corr[j - 1] and corr[j] >= corr[min(j + 1, len(corr) - 1)]:
            k, best = j, corr[j]
            break
    y0, y1, y2 = corr[max(k - 1, 0)], corr[k], corr[min(k + 1, len(corr) - 1)]
    den = y0 - 2 * y1 + y2
    off = 0.5 * (y0 - y2) / den if den else 0.0
    return sr / (k + lo + off), best


def analyse(path, kind, freq, tail, gate_off, aliases=(1.0,)):
    sr, x = load(path)
    peak = max(abs(v) for v in x)
    out = {"file": path, "sr": sr, "peak_db": db(peak), "fails": []}
    fail = out["fails"].append

    if peak < 1e-4:
        fail("silent")
        return out
    out["dc"] = abs(sum(x) / len(x)) / peak
    if out["dc"] > 0.05:
        fail("DC offset %.1f%% of peak" % (100 * out["dc"]))

    # Window: skip the first 40 ms so the strike transient does not pick the pitch.
    seg = window(x, sr, 0.04, 0.25)
    out["centroid"] = centroid(seg, sr)
    out["hf"] = hf_fraction(seg, sr)
    if out["hf"] > 0.02 and kind != "noise":
        fail("%.1f%% of energy above 0.40*SR (aliasing or a harsh top)" % (100 * out["hf"]))

    if kind == "harmonic":
        f0, c = autocorr_f0(seg, sr, max(40.0, freq * 0.4), min(4000.0, freq * 2.5))
        out["f0"], out["corr"] = f0, c
        # `aliases` are the ratios of the requested pitch at which a correct
        # instrument may legitimately PERIODICALLY repeat: an organ with its 16'
        # drawbar out has a waveform period one octave down (0.5) and is still
        # playing the key that was pressed.
        if not f0 or min(abs(f0 - freq * k) / (freq * k) for k in aliases) > 0.03:
            fail("f0 %.1f Hz is more than 3%% from the requested %.1f Hz" % (f0, freq))
        if c < 0.7:
            fail("periodicity %.2f < 0.7: not a tone" % c)

    # Tail: the last 15% against the loudest 50 ms.
    n50 = max(1, int(0.05 * sr))
    loud = max(rms(x[i:i + n50]) for i in range(0, max(1, len(x) - n50), n50 // 2))
    last = rms(x[int(len(x) * 0.85):])
    out["tail_db"] = db(last) - db(loud)
    if tail == "decay" and out["tail_db"] > -30.0:
        fail("has not rung out: tail is only %.1f dB below the loudest 50 ms" % -out["tail_db"])
    if tail == "sustain" and gate_off is not None:
        # After release the last 15% must be well down on the held part.
        held = rms(x[int(0.3 * gate_off * sr): int(gate_off * sr)])
        out["release_db"] = db(last) - db(held)
        if out["release_db"] > -25.0:
            fail("does not release: tail only %.1f dB below the held note" % -out["release_db"])
    return out


def main():
    a = sys.argv[1:]
    if a and a[0] == "--ratio":
        lo, hi, octaves = a[1], a[2], float(a[a.index("--octave-up") + 1])
        sr, x1 = load(lo)
        _, x2 = load(hi)
        c1 = centroid(window(x1, sr, 0.04), sr)
        c2 = centroid(window(x2, sr, 0.04), sr)
        want = 2.0 ** octaves
        r = c2 / max(c1, 1e-9)
        print("  centroid %.0f -> %.0f Hz  ratio %.2f (want ~%.2f)" % (c1, c2, r, want))
        # Loose on purpose: a filter or a fixed formant legitimately keeps
        # part of the spectrum still. The failure this catches is a freq
        # slider that is not wired at all (ratio 1.0).
        if r < 1.0 + 0.4 * (want - 1.0):
            print("  FAIL: octave up moved the spectrum by only x%.2f" % r)
            sys.exit(1)
        return
    path = a[0]

    def opt(name, default=None):
        return a[a.index(name) + 1] if name in a else default

    res = analyse(path, opt("--kind", "harmonic"), float(opt("--freq", "220")),
                  opt("--tail", "decay"),
                  float(opt("--gate-off")) if opt("--gate-off") else None,
                  tuple(float(v) for v in opt("--alias", "1.0").split(",")))
    if "--json" in a:
        print(json.dumps(res))
    else:
        bits = ["peak %.1f dBFS" % res["peak_db"]]
        for k, fmt in (("f0", "f0 %.1f Hz"), ("corr", "corr %.2f"), ("centroid", "centroid %.0f Hz"),
                       ("hf", "hf %.4f"), ("tail_db", "tail %.1f dB"), ("release_db", "release %.1f dB")):
            if k in res:
                bits.append(fmt % res[k])
        print("  %s: %s" % (path, ", ".join(bits)))
    for f in res["fails"]:
        print("  FAIL: %s" % f)
    sys.exit(1 if res["fails"] else 0)


main()
