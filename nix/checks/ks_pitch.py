# SPDX-License-Identifier: MIT
#
# ks_pitch.py — is a baked Faust pluck a PITCH, and is it the pitch asked for?
#
# usage: ks_pitch.py <file.wav> <expected_hz>
#
# Mean-removed, energy-normalised autocorrelation over 0.05..0.25 s: skip the
# lags before the correlation first goes negative (the main lobe), take the
# strongest lag after it. Plain Python on purpose — a numpy fetch for one loop
# is not worth it, and the segment is 6400 samples.
import struct
import sys
import wave


def load(path):
    w = wave.open(path)
    sr, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
    raw = w.readframes(w.getnframes())
    if sw != 2:
        sys.exit("ks_pitch: %s is %d-byte PCM, expected 16-bit" % (path, sw))
    s = struct.unpack("<%dh" % (len(raw) // 2), raw)
    return sr, list(s[::ch])


def main():
    path, want = sys.argv[1], float(sys.argv[2])
    sr, x = load(path)
    peak = max(abs(v) for v in x) or 1
    seg = x[int(0.05 * sr):int(0.25 * sr)]
    mean = sum(seg) / len(seg)
    seg = [v - mean for v in seg]
    energy = sum(v * v for v in seg) or 1.0
    lo, hi = int(sr / 2500), int(sr / 60)
    corr = []
    for lag in range(lo, hi):
        corr.append(sum(seg[i] * seg[i + lag] for i in range(len(seg) - lag)) / energy)
    first_neg = next((k for k, c in enumerate(corr) if c < 0), None)
    fails = 0
    if first_neg is None:
        print("  FAIL: autocorrelation never goes negative - no periodicity at all")
        fails += 1
        f0, best = 0.0, 0.0
    else:
        k = max(range(first_neg, len(corr)), key=lambda j: corr[j])
        f0, best = sr / (k + lo), corr[k]
    dc = abs(mean) / peak
    print("  %s: f0 %.1f Hz (want %.0f), corr %.2f, DC %.1f%% of peak %d"
          % (path, f0, want, best, 100 * dc, peak))
    if f0 and abs(f0 - want) / want > 0.05:
        print("  FAIL: pitch %.1f Hz is more than 5%% from %.0f Hz" % (f0, want))
        fails += 1
    if best < 0.8:
        print("  FAIL: periodicity %.2f < 0.8 - this is not a tone" % best)
        fails += 1
    if dc > 0.05:
        print("  FAIL: DC offset is %.1f%% of peak - the excitation is a step" % (100 * dc))
        fails += 1
    sys.exit(1 if fails else 0)


main()
