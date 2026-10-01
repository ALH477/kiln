#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# trim.py — set an instrument's output constant so its default gain peaks at
# -6 dBFS, the library's level convention (check.py wants -14..-1).
#
# usage: trim.py <name> [...]      (after tools/instruments/dev.sh has built render-<name>)
#
# Rewrites the LAST `gain * <number>` in dsp/instruments/<name>.dsp. Two passes,
# because a soft clip makes the first correction approximate. It changes a
# constant and nothing else; read the diff.
import json
import math
import os
import re
import struct
import subprocess
import sys
import wave

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
OUT = os.environ.get("KILN_INSTR_OUT", os.path.join(ROOT, ".instruments"))


def peak_db(name, inst, sr):
    wav = os.path.join(OUT, "trim_%s.wav" % name)
    cmd = [os.path.join(OUT, "bin", "render-" + name), "-o", wav, "-r", str(sr), "-d", str(inst["duration"])]
    params = dict(inst.get("params", {}))
    if inst.get("freqParam", True):
        params["freq"] = inst["freq"]
    for k, v in sorted(params.items()):
        cmd += ["-p", "%s=%s" % (k, v)]
    g = inst["gate"]
    cmd += ["-g", "gate:%s:%s" % (g["on"], g["off"])]
    subprocess.run(cmd, check=True, capture_output=True)
    w = wave.open(wav)
    s = struct.unpack("<%dh" % (w.getnframes() * w.getnchannels()), w.readframes(w.getnframes()))
    return 20 * math.log10(max(max(abs(v) for v in s), 1) / 32768.0)


def main():
    cat = json.load(open(os.path.join(ROOT, "dsp/instruments/catalogue.json")))
    for name in sys.argv[1:]:
        path = os.path.join(ROOT, "dsp/instruments", name + ".dsp")
        for _ in range(3):
            src = open(path).read()
            ms = list(re.finditer(r"gain \* ([0-9.]+)", src))
            m = ms[-1]
            db = peak_db(name, cat["instruments"][name], cat["sampleRate"])
            if abs(db + 6.0) < 0.7:
                break
            k = float(m.group(1)) * 10 ** ((-6.0 - db) / 20.0)
            src = src[:m.start(1)] + ("%.3g" % k) + src[m.end(1):]
            open(path, "w").write(src)
            subprocess.run([os.path.join(ROOT, "tools/instruments/dev.sh"), "--build-only", name], check=True, capture_output=True)
        print("%-14s peak %.1f dBFS  k=%s" % (name, db, re.findall(r"gain \* ([0-9.]+)", open(path).read())[-1]))


main()
