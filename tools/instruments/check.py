#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# check.py — hold every instrument in dsp/instruments/catalogue.json to what
# the catalogue says it is.
#
# usage: check.py <catalogue.json> <render-bin-dir> [--only name,name] [--keep DIR]
#        check.py --lint <catalogue.json> <dsp/instruments dir>
#
# <render-bin-dir> holds one `render-<name>` per instrument (what
# mkOfflineRenderer installs, or what tools/instruments/dev.sh builds). For each:
#
#   1. render the reference note and analyse it (analyse.py): pitch for the
#      harmonic ones, rung-out or released, no DC, no aliasing canary;
#   2. render one octave up. A harmonic instrument must land on 2f; an
#      inharmonic one (no f0 to measure) must move its whole SPECTRUM. Either
#      way this is the check that a freq slider is actually wired, which
#      dsp/ks.dsp once was not;
#   3. level: the default gain must land the peak between -14 and -1 dBFS, so
#      "gain" means the same thing across the whole library and a baked
#      instrument never trips mkBakedInstrument's clipping gate.
#
# A voice marked "live" is additionally held to the no-libm rule by
# nix/faust.nix, not here: this script only sees rendered audio.
import json
import os
import re
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))


def render(binary, out, freq, inst, sr, extra=None):
    cmd = [binary, "-o", out, "-r", str(sr), "-d", str(inst["duration"])]
    params = dict(inst.get("params", {}))
    params.update(extra or {})
    if inst.get("freqParam", True):
        params["freq"] = freq
    for k, v in sorted(params.items()):
        cmd += ["-p", "%s=%s" % (k, v)]
    g = inst.get("gate")
    if g:
        cmd += ["-g", "gate:%s:%s" % (g["on"], g["off"])]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        raise SystemExit("render failed: %s" % " ".join(cmd))


def run_analyse(path, inst, freq):
    cmd = [sys.executable, os.path.join(HERE, "analyse.py"), path,
           "--kind", inst["kind"], "--freq", str(freq),
           "--tail", inst.get("tail", "decay")]
    if inst.get("gate"):
        cmd += ["--gate-off", str(inst["gate"]["off"])]
    if inst.get("pitchAlias"):
        cmd += ["--alias", ",".join(["1.0"] + [str(v) for v in inst["pitchAlias"]])]
    r = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    return r.returncode == 0, r.stdout


# Namespaces a LIVE voice's own source may reach into. Everything else in the
# Faust standard library is, somewhere, a libm call (os.osc fills a table with
# sinf, fi.svf and fi.lowpass call tanf, ba.tau2pole calls expf, re.* and pm.*
# pull in both) and nix/faust.nix's symbol gate rejects the object. kiln.lib is
# the supported route to an oscillator or a filter; this lint names the mistake
# at the line, before a 40-minute toolchain build names it at the symbol.
LIVE_NS = {"ma", "ba", "no", "de", "kl"}
FAMILIES = {"strings", "keys", "mallets", "synth", "bass", "drums", "sfx", "ambient"}
KINDS = {"harmonic", "inharmonic", "noise"}


def lint(cat_path, dsp_dir):
    cat = json.load(open(cat_path))["instruments"]
    errs = []
    files = {f[:-4] for f in os.listdir(dsp_dir) if f.endswith(".dsp")}
    for n in sorted(files - set(cat)):
        errs.append("%s.dsp has no catalogue entry (it would never be built or checked)" % n)
    for n in sorted(set(cat) - files):
        errs.append("catalogue lists %s but %s.dsp does not exist" % (n, n))
    for n in sorted(files & set(cat)):
        inst = cat[n]
        src = open(os.path.join(dsp_dir, n + ".dsp")).read()
        if "SPDX-License-Identifier: MIT" not in src.split("\n", 3)[0]:
            errs.append("%s.dsp: first line must be the MIT SPDX identifier" % n)
        if not re.search(r"^// %s \u2014" % re.escape(n), src, re.M):
            errs.append("%s.dsp: header must start '// %s \u2014 <one line>' (INDEX.md reads it)" % (n, n))
        if inst["family"] not in FAMILIES:
            errs.append("%s: unknown family %r" % (n, inst["family"]))
        if inst["kind"] not in KINDS:
            errs.append("%s: unknown kind %r" % (n, inst["kind"]))
        for need in ('hslider("gain"', 'button("gate")'):
            if need not in src:
                errs.append("%s.dsp: standard control %s missing" % (n, need))
        if inst.get("freqParam", True) and 'hslider("freq"' not in src:
            errs.append("%s.dsp: catalogue says freqParam but no freq slider" % n)
        if inst.get("live"):
            code = "\n".join(l.split("//")[0] for l in src.split("\n"))
            for m in re.finditer(r"\b([a-z]{2})\.[A-Za-z_]", code):
                if m.group(1) not in LIVE_NS:
                    errs.append("%s.dsp is marked live but uses %s (libm in its constructor; use kl.*)"
                                % (n, m.group(0)))
    return errs


def main():
    if sys.argv[1] == "--lint":
        errs = lint(sys.argv[2], sys.argv[3])
        for e in errs:
            print("  FAIL: " + e)
        if errs:
            raise SystemExit(1)
        print("catalogue and sources agree")
        return
    cat_path, bindir = sys.argv[1], sys.argv[2]
    only = None
    keep = None
    if "--only" in sys.argv:
        only = set(sys.argv[sys.argv.index("--only") + 1].split(","))
    if "--keep" in sys.argv:
        keep = sys.argv[sys.argv.index("--keep") + 1]
        os.makedirs(keep, exist_ok=True)
    cat = json.load(open(cat_path))
    sr = cat["sampleRate"]
    work = keep or os.environ.get("TMPDIR", "/tmp")
    fails = []
    for name, inst in sorted(cat["instruments"].items()):
        if only and name not in only:
            continue
        print("== %s (%s, %s)" % (name, inst["family"], inst["kind"]))
        binary = os.path.join(bindir, "render-" + name)
        f = inst["freq"]
        a, b = os.path.join(work, name + ".wav"), os.path.join(work, name + "_up.wav")
        render(binary, a, f, inst, sr)
        want_ch = 2 if inst.get("stereo") else 1
        got_ch = wave.open(a).getnchannels()
        if got_ch != want_ch:
            print("  FAIL: renders %d channel(s), catalogue says %d (a stereo wav64 costs "
                  "TWO mixer channels; say so with \"stereo\": true or make it mono)" % (got_ch, want_ch))
            fails.append(name)
        ok, out = run_analyse(a, inst, f)
        if not ok:
            fails.append(name)
        # Level, from the analyse output's own number.
        peak = float(out.split("peak ")[1].split(" dBFS")[0]) if "peak " in out else -99.0
        if not (-14.0 <= peak <= -1.0):
            print("  FAIL: peak %.1f dBFS at default gain, want -14..-1" % peak)
            fails.append(name)
        if inst.get("freqParam", True):
            render(binary, b, f * 2, inst, sr)
            if inst["kind"] == "harmonic":
                # A harmonic tone proves `freq` is wired by landing on 2f.
                # (Its centroid is set by the excitation, not the pitch, so the
                # spectrum-moves test below would be wrong for it.)
                ok, _ = run_analyse(b, inst, f * 2)
                if not ok:
                    fails.append(name)
            else:
                r = subprocess.run([sys.executable, os.path.join(HERE, "analyse.py"), "--ratio", a, b,
                                    "--octave-up", "1"], capture_output=True, text=True)
                sys.stdout.write(r.stdout)
                if r.returncode != 0:
                    fails.append(name)
    if fails:
        print("\ninstruments FAILED: %s" % ", ".join(sorted(set(fails))))
        sys.exit(1)
    print("\nall instruments pass")


main()
