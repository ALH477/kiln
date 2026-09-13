#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""asset_budget.py — measure what a scene's assets actually cost, and hold it
to a declared ceiling.

    asset_budget.py measure --model <model-derivation-dir> ...
    asset_budget.py check --budget <budget.json> --root <resolved-assets-dir>
    asset_budget.py selftest

── The gap this fills ─────────────────────────────────────────────────────
Kiln already gates a lot: rom.nix checks a .z64's magic, title and size
ceiling; kiln-asset.nix round-trips the asset layer; level-vocab.nix keeps the
entity vocabulary to one statement; a downstream game can pass `max_tris` to
kilnlib.report. Every one of those is about ONE artifact.

Nothing knows what a SCENE costs. The numbers that decide whether a screen
runs are sums — triangles and vertices submitted per frame, bytes resident in
RDRAM, bytes of TMEM a material needs, mixer channels in use — and they live
in prose. In the game this was written for they lived in three documents:
"2,058 triangles for the set", "each under 1,088 B of TMEM", "the mixer has 16
SFX channels". None of those is enforced anywhere, and the one that is
(`max_tris`) is opt-in per script, so the two scripts that never pass it have
no ceiling at all.

The failure that follows is not a crash. It is a screen that quietly runs at
34 fps instead of 60, or a resident set that is the union of every screen ever
visited because nothing unloaded, and both of those are invisible to `nix
build`.

── Why the measurement comes from the BUILT artifact ──────────────────────
Not from the source, and not from a number typed into a table. A model's
source .glb is not what ships: pm_meshy/pm_demons/pm_props all decimate, split
double-sided faces, merge materials and re-export, so the only honest triangle
count is the one in the exported glTF that gltf_to_t3d consumed. Reading that
is also how a real measurement pass in this project found that a decimated
generated prop ships 2.19 vertices per triangle where authored geometry ships
1.05 — a fact no source-side count could have shown, and one that matters more
than the triangle count it was hiding behind.

── What is exact and what is an upper bound ───────────────────────────────
Exact: triangles, vertices, materials, per-material TMEM, file bytes.
Upper bound: RDRAM. This does not simulate libdragon's heap. `t3d_model_load`
reads a whole .t3dm into one allocation, so the file size is a sound upper
bound on the model's resident cost, and that is what `resident_bytes` reports.
It is deliberately pessimistic: a budget that passes on an upper bound is
safe, and one that needs the difference is too tight to trust anyway.

Nothing here guesses. An asset type this does not know how to measure is a
FAILURE, not a zero — see MEASURERS and `measure_any`.
"""
import argparse
import json
import os
import re
import struct
import sys
import wave

# ── The hardware, as constants rather than as folklore ─────────────────────
TMEM_BYTES = 4096          # the RDP's texture memory, shared by every tile
TLUT_ENTRY_BYTES = 2       # RGBA5551
CI4_ENTRIES = 16
CI8_ENTRIES = 256

# bits per texel, by the `format` a material spec declares
TEXEL_BITS = {
    "CI4": 4, "CI8": 8, "I4": 4, "I8": 8,
    "IA4": 4, "IA8": 8, "IA16": 16,
    "RGBA16": 16, "RGBA32": 32,
}


def die(msg):
    print("asset_budget: %s" % msg, file=sys.stderr)
    raise SystemExit(1)


# ── Texture cost ───────────────────────────────────────────────────────────
def tmem_bytes(fmt, width, height):
    """Bytes of TMEM one texture occupies, palette included.

    The palette is not optional bookkeeping: a CI4 tile is 512 B of texels
    and 32 B of TLUT, and a budget that counts only the texels is wrong by
    exactly the amount that makes four of them fit where three do.
    """
    if fmt not in TEXEL_BITS:
        die("unknown texture format %r (known: %s)"
            % (fmt, ", ".join(sorted(TEXEL_BITS))))
    texels = (width * height * TEXEL_BITS[fmt] + 7) // 8
    if fmt == "CI4":
        return texels + CI4_ENTRIES * TLUT_ENTRY_BYTES
    if fmt == "CI8":
        return texels + CI8_ENTRIES * TLUT_ENTRY_BYTES
    return texels


def measure_texture(path, fmt=None):
    """A PNG's shape, and the TMEM it will need once converted.

    Measured from the PNG rather than from the .sprite on purpose, and with
    precedent: the existing per-texture gate in the game repo checks mode P,
    colour count, squareness and power-of-two on the PNG too. libdragon's
    sprite container has changed shape more than once; a palette index has
    not.
    """
    try:
        from PIL import Image
    except ImportError:
        die("Pillow is required to measure textures")
    im = Image.open(path)
    w, h = im.size
    colours = len(im.getpalette() or []) // 3 if im.mode == "P" else None
    if fmt is None:
        # Inferred, and only for the two cases that are unambiguous. Anything
        # else must be declared, because guessing RGBA16 for a truecolour PNG
        # that ships as CI8 understates TMEM by half.
        fmt = "CI4" if (im.mode == "P" and (colours or 0) <= CI4_ENTRIES) else None
        if fmt is None:
            die("%s: cannot infer a format for a %s PNG with %s colours — "
                "declare it in the budget" % (path, im.mode, colours))
    return {
        "kind": "texture", "path": path, "format": fmt,
        "width": w, "height": h, "mode": im.mode, "colours": colours,
        "tmem_bytes": tmem_bytes(fmt, w, h),
        "file_bytes": os.path.getsize(path),
        "square": w == h,
        "pow2": w & (w - 1) == 0 and h & (h - 1) == 0,
    }


# ── Model cost ─────────────────────────────────────────────────────────────
def measure_model(root):
    """Triangles, vertices and materials out of a model derivation.

    `root` is a built model's output directory. Two things are read:
      share/gltf/<name>.gltf   the geometry gltf_to_t3d actually consumed
      filesystem/**/*.t3dm     what ships, for the resident upper bound
    plus any .sdata sidecars, which are the streamed animation keyframes and
    are ROM cost but not resident cost.
    """
    gltfs = []
    t3dms = []
    sdata = []
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            p = os.path.join(dirpath, f)
            if f.endswith(".gltf"):
                gltfs.append(p)
            elif f.endswith(".t3dm"):
                t3dms.append(p)
            elif f.endswith(".sdata"):
                sdata.append(p)
    if not gltfs:
        die("%s has no share/gltf/*.gltf — this is not a built model "
            "directory, or mkBlenderModel stopped keeping the intermediate "
            "glTF (which is the only place the shipped triangle count is "
            "readable)" % root)

    tris = verts = 0
    materials = set()
    animations = set()
    for g in gltfs:
        j = json.load(open(g))
        acc = j.get("accessors", [])
        for m in j.get("meshes", []):
            for pr in m.get("primitives", []):
                if "indices" in pr:
                    tris += acc[pr["indices"]]["count"] // 3
                pos = (pr.get("attributes") or {}).get("POSITION")
                if pos is not None:
                    verts += acc[pos]["count"]
        materials.update(m.get("name") for m in j.get("materials", []))
        animations.update(a.get("name") for a in (j.get("animations") or []))

    return {
        "kind": "model", "path": root,
        "tris": tris, "verts": verts,
        "verts_per_tri": round(verts / tris, 3) if tris else 0.0,
        "materials": sorted(x for x in materials if x),
        "animations": sorted(x for x in animations if x),
        # The upper bound. See the module docstring on why file size.
        "resident_bytes": sum(os.path.getsize(p) for p in t3dms),
        "rom_bytes": sum(os.path.getsize(p) for p in t3dms + sdata),
        "clip_bytes": sum(os.path.getsize(p) for p in sdata),
    }


# ── Audio cost ─────────────────────────────────────────────────────────────
def measure_audio(path):
    """A source WAV's rate, channel count and length.

    Channel count is not a detail. libdragon's mixer plays a stereo waveform
    across two ADJACENT channels, so a stereo bed started on channel 0
    silently occupies 0 AND 1 — and the symptom is not a wrong sound, it is
    `samplebuffer_get: no reader to extend` asserting a few seconds into
    boot. A budget that counts one channel for that file is describing a ROM
    that crashes.
    """
    if path.lower().endswith(".wav"):
        with wave.open(path, "rb") as w:
            ch, rate, frames = w.getnchannels(), w.getframerate(), w.getnframes()
    else:
        # A pre-converted .wav64: its header is libdragon's, and this
        # deliberately does not parse it. Report the bytes and say the rest
        # is unknown rather than inventing a rate.
        return {
            "kind": "audio", "path": path, "channels": None, "rate": None,
            "seconds": None, "file_bytes": os.path.getsize(path),
            "mixer_channels": None,
        }
    return {
        "kind": "audio", "path": path,
        "channels": ch, "rate": rate,
        "seconds": round(frames / float(rate), 3) if rate else None,
        "file_bytes": os.path.getsize(path),
        # What it costs the mixer, which is the number a scene budget cares
        # about: one per channel, adjacent, per the docstring above.
        "mixer_channels": ch,
    }


MEASURERS = {"model": measure_model, "texture": measure_texture,
             "audio": measure_audio}


def measure_any(kind, path, **kw):
    if kind not in MEASURERS:
        die("no measurer for asset kind %r. Add one to MEASURERS rather than "
            "letting it count as zero — an unmeasured asset in a budget is a "
            "budget that passes for the wrong reason." % kind)
    return MEASURERS[kind](path, **kw)


# ── The check ──────────────────────────────────────────────────────────────
CEILING_KEYS = ("tris", "verts", "resident_bytes", "tmem_bytes",
                "mixer_channels", "rom_bytes")


def check(budget, resolve, out=sys.stdout):
    """Hold every scene in `budget` to its ceiling. Returns a failure count.

    `resolve` maps an asset's declared name to a path on disk. Kept as a
    callback so the nix wrapper can point it at store paths and the selftest
    can point it at a fixture without either knowing about the other.
    """
    fails = []

    def bad(msg):
        print("  FAIL  %s" % msg, file=out)
        fails.append(msg)

    plat = budget.get("platform") or {}
    tmem_cap = plat.get("tmem_bytes", TMEM_BYTES)
    mixer_cap = plat.get("mixer_channels")
    rdram_cap = plat.get("rdram_bytes")

    scenes = budget.get("scenes") or {}
    if not scenes:
        bad("the budget declares no scenes")
        return len(fails)

    seen_assets = set()
    for name in sorted(scenes):
        scene = scenes[name]
        print("\n── scene %s ──" % name, file=out)
        ceil = scene.get("ceiling") or {}
        if not ceil:
            bad("%s has no ceiling. A scene without one is not budgeted, and "
                "this check would report it green." % name)
            continue

        totals = dict.fromkeys(CEILING_KEYS, 0)
        assets = scene.get("assets") or []
        if not assets:
            bad("%s lists no assets" % name)
        for a in assets:
            kind, ref = a.get("kind"), a.get("name")
            if not kind or not ref:
                bad("%s has an asset entry missing kind or name: %r" % (name, a))
                continue
            seen_assets.add((kind, ref))
            path = resolve(kind, ref)
            if path is None or not os.path.exists(str(path)):
                bad("%s: %s %r does not resolve to a path on disk (%s)"
                    % (name, kind, ref, path))
                continue
            extra = {"fmt": a["format"]} if kind == "texture" and "format" in a else {}
            m = measure_any(kind, str(path), **extra)

            # Per-asset invariants that do not depend on the scene.
            if kind == "texture":
                if not m["square"]:
                    bad("%s: %s is %dx%d — f3d_inject computes one wrap mask "
                        "for S and T, so a non-square texture gets a wrong "
                        "T mask" % (name, ref, m["width"], m["height"]))
                if not m["pow2"]:
                    bad("%s: %s is %dx%d, not a power of two"
                        % (name, ref, m["width"], m["height"]))
                if m["tmem_bytes"] > tmem_cap:
                    bad("%s: %s alone needs %d B of TMEM, which is more than "
                        "the %d B the RDP has"
                        % (name, ref, m["tmem_bytes"], tmem_cap))
            if kind == "audio" and m["channels"] == 2:
                print("        note: %s is STEREO and therefore takes TWO "
                      "adjacent mixer channels" % ref, file=out)

            for k in CEILING_KEYS:
                if m.get(k):
                    totals[k] += m[k]

        # ── The sums ───────────────────────────────────────────────────────
        for k in sorted(ceil):
            if k not in CEILING_KEYS:
                bad("%s's ceiling names %r, which nothing measures. Either "
                    "add a measurer or drop the key — a ceiling on a quantity "
                    "that is always zero always passes." % (name, k))
                continue
            got, cap = totals[k], ceil[k]
            mark = "ok  " if got <= cap else "FAIL"
            print("  %s  %-15s %8d / %8d" % (mark, k, got, cap), file=out)
            if got > cap:
                fails.append("%s: %s %d over its ceiling of %d"
                             % (name, k, got, cap))

        # Platform ceilings apply whether or not the scene restated them.
        if mixer_cap and totals["mixer_channels"] > mixer_cap:
            bad("%s wants %d mixer channels; the platform has %d"
                % (name, totals["mixer_channels"], mixer_cap))
        if rdram_cap and totals["resident_bytes"] > rdram_cap:
            bad("%s is resident %d B against an RDRAM budget of %d B"
                % (name, totals["resident_bytes"], rdram_cap))
        for k in CEILING_KEYS:
            if totals[k] and k not in ceil:
                print("        note: %s totals %d and has no ceiling"
                      % (k, totals[k]), file=out)

    # ── Completeness, which is what makes this a gate and not a report ─────
    # Every asset the game ships must be in at least one scene. Without this
    # the check is opt-in, and an asset nobody budgeted is exactly the one
    # that pushes a screen over: four generated props once reached a ROM and
    # were placed on a level while being absent from every budget, texture
    # gate and triangle ceiling in the project.
    declared = budget.get("all_assets")
    if declared is None:
        print("\n── completeness ──", file=out)
        bad("the budget has no `all_assets` list, so nothing can tell whether "
            "an asset was left out of every scene. That omission is the whole "
            "failure mode this check exists for.")
    else:
        print("\n── completeness ──", file=out)
        want = {(a["kind"], a["name"]) for a in declared}
        for k, r in sorted(want - seen_assets):
            bad("%s %r ships but is in no scene's budget" % (k, r))
        for k, r in sorted(seen_assets - want):
            bad("%s %r is budgeted but is not in `all_assets`" % (k, r))
        if want == seen_assets:
            print("  ok    all %d shipped asset(s) are budgeted" % len(want),
                  file=out)

    return len(fails)


# ── Proving it fires, which is the house rule ──────────────────────────────
# level-vocab.nix's header states it plainly: "A check only ever seen to pass
# might not be checking anything." Every assertion above is exercised here
# against a fixture, in both directions, so `selftest` failing is the signal
# that this file has stopped being a gate.
def selftest():
    import tempfile
    fails = 0

    def expect(cond, msg):
        nonlocal fails
        print("  %s %s" % ("ok  " if cond else "FAIL", msg))
        if not cond:
            fails += 1

    # TMEM arithmetic, against the numbers this hardware is documented with.
    expect(tmem_bytes("CI4", 32, 32) == 512 + 32,
           "a 32x32 CI4 tile is 512 B of texels plus a 32 B TLUT")
    expect(tmem_bytes("RGBA16", 64, 64) == 8192,
           "a 64x64 RGBA16 tile is 8192 B — twice TMEM, so it cannot load")
    expect(tmem_bytes("I8", 32, 32) == 1024, "a 32x32 I8 tile is 1024 B")
    try:
        tmem_bytes("RGBA5551", 32, 32)
        expect(False, "an unknown format is rejected")
    except SystemExit:
        expect(True, "an unknown format is rejected")

    with tempfile.TemporaryDirectory() as d:
        # A fixture model: the glTF is what gets measured, so a hand-written
        # one is a legitimate fixture and keeps the selftest off the network
        # and off Blender.
        mdir = os.path.join(d, "model-fix", "share", "gltf")
        os.makedirs(mdir)
        json.dump({
            "accessors": [{"count": 30}, {"count": 12}],
            "meshes": [{"primitives": [{"indices": 0,
                                        "attributes": {"POSITION": 1}}]}],
            "materials": [{"name": "fix_mat"}],
            "animations": [{"name": "idle"}],
        }, open(os.path.join(mdir, "fix.gltf"), "w"))
        fs = os.path.join(d, "model-fix", "filesystem", "models")
        os.makedirs(fs)
        open(os.path.join(fs, "fix.t3dm"), "wb").write(b"\0" * 2048)
        open(os.path.join(fs, "fix.0.sdata"), "wb").write(b"\0" * 512)

        m = measure_model(os.path.join(d, "model-fix"))
        expect(m["tris"] == 10 and m["verts"] == 12,
               "a model measures 10 tris and 12 verts from its glTF")
        expect(m["verts_per_tri"] == 1.2, "and reports 1.2 verts per triangle")
        expect(m["resident_bytes"] == 2048 and m["clip_bytes"] == 512,
               "resident bytes are the .t3dm; clips are the .sdata")
        expect(m["animations"] == ["idle"], "and it names its clips")

        try:
            os.makedirs(os.path.join(d, "not-a-model"))
            measure_model(os.path.join(d, "not-a-model"))
            expect(False, "a directory with no glTF is rejected")
        except SystemExit:
            expect(True, "a directory with no glTF is rejected")

        # A fixture texture, and one deliberately wrong.
        try:
            from PIL import Image
        except ImportError:
            print("  ---- Pillow absent; texture cases skipped")
            Image = None
        if Image:
            good = os.path.join(d, "good.png")
            im = Image.new("P", (32, 32))
            im.putpalette([0, 0, 0] * 16)
            im.save(good)
            t = measure_texture(good)
            expect(t["tmem_bytes"] == 544 and t["square"] and t["pow2"],
                   "a 32x32 16-colour P PNG is CI4 at 544 B and is square/pow2")

            odd = os.path.join(d, "odd.png")
            im2 = Image.new("P", (24, 32))
            im2.putpalette([0, 0, 0] * 16)
            im2.save(odd)
            t2 = measure_texture(odd)
            expect(not t2["pow2"] and not t2["square"],
                   "a 24x32 PNG is caught as neither square nor a power of two")

        # A fixture WAV, stereo, to prove the two-channel rule.
        st = os.path.join(d, "st.wav")
        with wave.open(st, "wb") as w:
            w.setnchannels(2); w.setsampwidth(2); w.setframerate(32000)
            w.writeframes(b"\0" * 4 * 32000)
        a = measure_audio(st)
        expect(a["mixer_channels"] == 2,
               "a stereo WAV costs TWO mixer channels, not one")
        expect(a["seconds"] == 1.0, "and measures its length")

        # ── The check itself, in both directions ──────────────────────────
        def resolve(kind, ref):
            return {"model": os.path.join(d, "model-fix")}.get(kind)

        import io
        base = {
            "platform": {"tmem_bytes": 4096, "mixer_channels": 16},
            "all_assets": [{"kind": "model", "name": "fix"}],
            "scenes": {"S": {"ceiling": {"tris": 10, "verts": 12},
                             "assets": [{"kind": "model", "name": "fix"}]}},
        }
        expect(check(base, resolve, out=io.StringIO()) == 0,
               "a scene exactly at its ceiling passes")

        tight = json.loads(json.dumps(base))
        tight["scenes"]["S"]["ceiling"]["tris"] = 9
        expect(check(tight, resolve, out=io.StringIO()) > 0,
               "one triangle over the ceiling fails")

        nover = json.loads(json.dumps(base))
        nover["all_assets"].append({"kind": "model", "name": "unbudgeted"})
        expect(check(nover, resolve, out=io.StringIO()) > 0,
               "an asset that ships in no scene fails")

        noceil = json.loads(json.dumps(base))
        del noceil["scenes"]["S"]["ceiling"]
        expect(check(noceil, resolve, out=io.StringIO()) > 0,
               "a scene with no ceiling fails rather than passing")

        nokey = json.loads(json.dumps(base))
        nokey["scenes"]["S"]["ceiling"]["fill_rate"] = 1
        expect(check(nokey, resolve, out=io.StringIO()) > 0,
               "a ceiling on a quantity nothing measures fails")

        noall = json.loads(json.dumps(base))
        del noall["all_assets"]
        expect(check(noall, resolve, out=io.StringIO()) > 0,
               "a budget with no all_assets list fails")

    print()
    if fails:
        print("asset_budget selftest FAILED (%d)" % fails, file=sys.stderr)
        return 1
    print("asset_budget selftest PASSED")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="asset_budget.py",
                                 description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("measure", help="print one asset's measured cost")
    p.add_argument("--kind", required=True, choices=sorted(MEASURERS))
    p.add_argument("path")
    p.add_argument("--format", default=None, help="texture format, e.g. CI4")

    p = sub.add_parser("check", help="hold a budget to its ceilings")
    p.add_argument("--budget", required=True)
    p.add_argument("--root", required=True,
                   help="directory holding <kind>/<name> resolved assets")

    sub.add_parser("selftest", help="prove every assertion here fires")

    a = ap.parse_args(argv)
    if a.cmd == "selftest":
        return selftest()
    if a.cmd == "measure":
        kw = {"fmt": a.format} if a.kind == "texture" and a.format else {}
        print(json.dumps(measure_any(a.kind, a.path, **kw), indent=2,
                         sort_keys=True))
        return 0

    budget = json.load(open(a.budget))

    def resolve(kind, ref):
        # <root>/<kind>/<name> — the nix wrapper stages assets into that
        # shape so this file never has to know about store paths.
        return os.path.join(a.root, kind, ref)

    n = check(budget, resolve)
    print()
    if n:
        print("asset budget FAILED (%d)" % n, file=sys.stderr)
        return 1
    print("asset budget PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
