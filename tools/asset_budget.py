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
import io
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
# ── Detail tiers, and the vertex-load batch ────────────────────────────────
# A model may carry LOD tiers as extra OBJECTS inside the one .t3dm, named
# `foo`, `foo.lod1`, `foo.lod2` — see kiln_detail.h. Only one tier of a given
# base is ever drawn, so the PER-FRAME cost of a model is its tier-0 objects
# alone, while ROM and resident cost is all of them. A budget that summed every
# tier would refuse a model that is cheaper on screen than the one it replaced,
# which is exactly backwards.
VERT_CACHE = 70   # gltf_to_t3d MAX_VERTEX_COUNT (structs.h:293)


def tier_of(name):
    """Detail tier from an object name: `foo.lod2` -> 2, anything else -> 0.

    Deliberately strict. `foo.001` is Blender's own collision suffix and is
    tier 0, not tier 1 — treating it as a tier would silently drop geometry
    from the frame the moment two objects were given the same name.
    """
    if not name:
        return 0
    base, _, suffix = name.rpartition(".")
    if not base or not suffix.startswith("lod"):
        return 0
    digits = suffix[3:]
    if not digits or not digits.isdigit():
        return 0
    return int(digits)


def parts_for(verts):
    """Vertex-load batches an object is split into — one RSP load each."""
    return max(1, -(-verts // VERT_CACHE))


def gltf_objects(path):
    """Per-object emitted geometry out of a glTF.

    This is the only place the SHIPPED vertex count is legible: Blender's
    len(mesh.vertices) is not it (the exporter splits a vertex per distinct
    position/normal/colour/uv), and the .t3dm is inside a DCA container by the
    time anyone could look. One entry per glTF PRIMITIVE, because that is one
    T3DObject (gltf_importer parser.cpp:181-183).
    """
    j = json.load(open(path))
    acc = j.get("accessors", [])
    nodes = j.get("nodes", [])
    mats = j.get("materials", [])

    # A primitive's T3DObject takes the NODE's name, not the mesh's.
    name_of_mesh = {}
    for n in nodes:
        if n.get("mesh") is not None and n.get("name"):
            name_of_mesh.setdefault(n["mesh"], n["name"])

    out = []
    for mi, m in enumerate(j.get("meshes", [])):
        name = name_of_mesh.get(mi) or m.get("name") or "mesh%d" % mi
        for pr in m.get("primitives", []):
            pos = (pr.get("attributes") or {}).get("POSITION")
            verts = acc[pos]["count"] if pos is not None else 0
            if "indices" in pr:
                tris = acc[pr["indices"]]["count"] // 3
            else:
                tris = verts // 3
            mi_ = pr.get("material")
            out.append({
                "name": name,
                "material": (mats[mi_].get("name") if mi_ is not None
                             and mi_ < len(mats) else None),
                "verts": verts,
                "tris": tris,
                "parts": parts_for(verts),
                "tier": tier_of(name),
            })
    out.sort(key=lambda o: (o["name"], o["material"] or ""))
    return out


def measure_model(root, objects=None):
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

    found = []
    materials = set()
    animations = set()
    for g in gltfs:
        found.extend(gltf_objects(g))
        j = json.load(open(g))
        materials.update(m.get("name") for m in j.get("materials", []))
        animations.update(a.get("name") for a in (j.get("animations") or []))

    # ── What one instance submits ─────────────────────────────────────────
    # Tier 0 only: a model's other tiers are ROM and resident cost, never
    # drawn at the same time as tier 0.
    #
    # `objects` narrows it further, and exists because a model is not always
    # drawn whole. PetaByte Madness' palms .t3dm is a prop ATLAS — sixteen
    # objects, from which the flyover picks ten by name with
    # t3d_model_get_object. Charging a scene for all sixteen would overstate it
    # by more than a factor of two, and charging it for one would understate it
    # tenfold; naming the objects is the only honest option.
    drawn = [o for o in found if o["tier"] == 0]
    if objects is not None:
        have = {o["name"] for o in found}
        missing = [n for n in objects if n not in have]
        if missing:
            die("%s: budget names object(s) %s, which this model does not "
                "contain (it has: %s). An object that does not resolve would "
                "otherwise count as zero triangles."
                % (root, ", ".join(repr(n) for n in missing),
                   ", ".join(sorted(have))))
        want = set(objects)
        drawn = [o for o in drawn if o["name"] in want]
    tris = sum(o["tris"] for o in drawn)
    verts = sum(o["verts"] for o in drawn)
    parts = sum(o["parts"] for o in drawn)
    tiers = {}
    for o in found:
        t = tiers.setdefault(str(o["tier"]),
                             {"objects": 0, "tris": 0, "verts": 0, "parts": 0})
        t["objects"] += 1
        t["tris"] += o["tris"]
        t["verts"] += o["verts"]
        t["parts"] += o["parts"]

    return {
        "kind": "model", "path": root,
        # tris/verts are what one instance submits in a frame: tier 0 only.
        "tris": tris, "verts": verts,
        "parts": parts,
        "objects": len(drawn),
        "verts_per_tri": round(verts / tris, 3) if tris else 0.0,
        # Every tier, which is what the ROM holds.
        "tris_all": sum(o["tris"] for o in found),
        "verts_all": sum(o["verts"] for o in found),
        "tiers": tiers,
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
# ── Per-frame cost versus resident cost ────────────────────────────────────
# These two are summed differently and conflating them is how a budget ends up
# describing a ROM nobody has.
#
# Per-frame keys scale with how many times a thing is DRAWN: the flyover puts
# four bone idols on the island, so it pays four idols' worth of vertices. An
# entry's `count` multiplies these.
#
# Resident keys are what the thing costs to HOLD, and that does not change with
# the instance count — the same T3DModel is drawn from four matrices. So they
# are counted once per distinct asset in a scene, however many entries name it.
PER_FRAME_KEYS = ("tris", "verts", "parts", "objects")
RESIDENT_KEYS = ("resident_bytes", "rom_bytes", "tmem_bytes", "mixer_channels")
CEILING_KEYS = PER_FRAME_KEYS + RESIDENT_KEYS


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
        counted_once = set()
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
            if kind == "model" and a.get("objects"):
                extra["objects"] = a["objects"]
            m = measure_any(kind, str(path), **extra)

            count = a.get("count", 1)
            if not isinstance(count, int) or count < 1:
                bad("%s: %s %r has count %r — an instance count must be a "
                    "positive integer" % (name, kind, ref, count))
                continue

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

            once_key = (kind, ref, a.get("format"))
            for k in CEILING_KEYS:
                v = m.get(k)
                if not v:
                    continue
                if k in PER_FRAME_KEYS:
                    totals[k] += v * count
                elif once_key not in counted_once:
                    totals[k] += v
            counted_once.add(once_key)
            if count > 1 or a.get("objects"):
                print("        note: %s x%d%s -> %d tri %d vert %d part"
                      % (ref, count,
                         (" " + ",".join(a["objects"])) if a.get("objects") else "",
                         m.get("tris", 0) * count, m.get("verts", 0) * count,
                         m.get("parts", 0) * count), file=out)

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

        # ── Detail tiers ──────────────────────────────────────────────────
        # `foo.001` is Blender's collision suffix, not tier 1. Reading it as a
        # tier would drop real geometry out of the per-frame figure and the
        # budget would go green by losing track of it.
        want_tiers = {"foo": 0, "foo.lod1": 1, "foo.lod2": 2, "foo.lod10": 10,
                      "foo.001": 0, "lod1": 0, "foo.lodX": 0, "foo.lod": 0,
                      "": 0, "foo.bar.lod3": 3}
        wrong = {n: tier_of(n) for n, w in want_tiers.items()
                 if tier_of(n) != w}
        expect(not wrong,
               "tier_of reads .lodN and nothing else (%s)"
               % ("all ok" if not wrong else wrong))

        expect(parts_for(0) == 1 and parts_for(VERT_CACHE) == 1
               and parts_for(VERT_CACHE + 1) == 2,
               "a vertex load covers VERT_CACHE verts, and one more is two loads")

        # A tiered model: 12 tris drawn, 20 in the ROM. The per-frame figure
        # must be the tier-0 half, or a model that got CHEAPER on screen would
        # be refused for the tiers that made it so.
        tdir = os.path.join(d, "model-tier", "share", "gltf")
        os.makedirs(tdir)
        json.dump({
            "accessors": [{"count": 36}, {"count": 20},
                          {"count": 24}, {"count": 9}],
            "nodes": [{"name": "hero", "mesh": 0},
                      {"name": "hero.lod1", "mesh": 1}],
            "meshes": [{"primitives": [{"indices": 0,
                                        "attributes": {"POSITION": 1}}]},
                       {"primitives": [{"indices": 2,
                                        "attributes": {"POSITION": 3}}]}],
            "materials": [{"name": "hero_mat"}],
        }, open(os.path.join(tdir, "tier.gltf"), "w"))
        tfs = os.path.join(d, "model-tier", "filesystem", "models")
        os.makedirs(tfs)
        open(os.path.join(tfs, "tier.t3dm"), "wb").write(b"\0" * 1024)

        mt = measure_model(os.path.join(d, "model-tier"))
        expect(mt["tris"] == 12 and mt["verts"] == 20,
               "a tiered model's per-frame cost is its tier-0 objects alone")
        expect(mt["tris_all"] == 20 and mt["verts_all"] == 29,
               "while every tier counts towards what the ROM holds")
        expect(mt["objects"] == 1 and sorted(mt["tiers"]) == ["0", "1"],
               "and each tier is reported separately")

        def resolve_tier(kind, ref):
            return {"model": os.path.join(d, "model-tier")}.get(kind)

        import io as _io
        tiered = {
            "platform": {"tmem_bytes": 4096, "mixer_channels": 16},
            "all_assets": [{"kind": "model", "name": "tier"}],
            "scenes": {"S": {"ceiling": {"tris": 12, "verts": 20},
                             "assets": [{"kind": "model", "name": "tier"}]}},
        }
        expect(check(tiered, resolve_tier, out=_io.StringIO()) == 0,
               "a scene passes on the drawn tier even though every tier "
               "together would not fit")

        # ── Instance count and object subsets ─────────────────────────────
        # Four idols on the island cost four idols of vertices but one idol of
        # RDRAM. A budget that scaled both, or neither, would be wrong in
        # opposite directions.
        four = json.loads(json.dumps(tiered))
        four["scenes"]["S"]["assets"][0]["count"] = 4
        four["scenes"]["S"]["ceiling"] = {"tris": 48, "verts": 80,
                                          "resident_bytes": 1024}
        expect(check(four, resolve_tier, out=_io.StringIO()) == 0,
               "count multiplies per-frame cost but not resident bytes")

        threes = json.loads(json.dumps(four))
        threes["scenes"]["S"]["ceiling"]["tris"] = 47
        expect(check(threes, resolve_tier, out=_io.StringIO()) > 0,
               "and one triangle short of four instances still fails")

        badcount = json.loads(json.dumps(tiered))
        badcount["scenes"]["S"]["assets"][0]["count"] = 0
        expect(check(badcount, resolve_tier, out=_io.StringIO()) > 0,
               "a count of zero is rejected rather than silently free")

        # Naming objects narrows a prop atlas to the ones actually drawn.
        subset = json.loads(json.dumps(tiered))
        subset["scenes"]["S"]["assets"][0]["objects"] = ["hero"]
        expect(check(subset, resolve_tier, out=_io.StringIO()) == 0,
               "naming a model's objects narrows it to what is drawn")

        ghost = json.loads(json.dumps(tiered))
        ghost["scenes"]["S"]["assets"][0]["objects"] = ["no_such_object"]
        try:
            check(ghost, resolve_tier, out=_io.StringIO())
            expect(False, "an object the model does not contain fails")
        except SystemExit:
            expect(True, "an object the model does not contain fails")

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
    p.add_argument("--json", action="store_true",
                   help="print a tools/schema/report.schema.json report instead")

    sub.add_parser("selftest", help="prove every assertion here fires")

    ps = sub.add_parser("stats", help="per-object emitted geometry of a glTF")
    ps.add_argument("gltf")

    a = ap.parse_args(argv)
    if a.cmd == "selftest":
        return selftest()
    if a.cmd == "stats":
        objs = gltf_objects(a.gltf)
        tv = tt = tp = 0
        for o in objs:
            tv += o["verts"]
            tt += o["tris"]
            tp += o["parts"]
            note = "  (%d vertex loads)" % o["parts"] if o["parts"] > 1 else ""
            tier = "  lod%d" % o["tier"] if o["tier"] else ""
            print("  [T3D] %-24s %5d vert %4d tri %3d part  %-16s%s%s"
                  % (o["name"][:24], o["verts"], o["tris"], o["parts"],
                     (o["material"] or "-")[:16], tier, note))
        ratio = " (%.2f vert/tri)" % (tv / tt) if tt else ""
        print("  [T3D] %-24s %5d vert %4d tri %3d part  %d object(s)%s"
              % ("TOTAL", tv, tt, tp, len(objs), ratio))
        return 0
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

    if a.json:
        # The same check, its human output kept as the log and its FAIL lines
        # turned into findings — one implementation of the rules, not two.
        buf = io.StringIO()
        n = check(budget, resolve, out=buf)
        text = buf.getvalue()
        errors = [{"code": "BUDGET", "msg": line.strip()[len("FAIL"):].strip()}
                  for line in text.splitlines() if line.strip().startswith("FAIL")]
        if n and not errors:
            errors = [{"code": "BUDGET", "msg": f"{n} failure(s)"}]
        print(json.dumps({"tool": "asset-budget", "version": 1, "ok": n == 0, "errors": errors,
                          "notes": [], "subject": a.budget, "metrics": {"failures": n, "log": text}},
                         indent=1))
        return 1 if n else 0

    n = check(budget, resolve)
    print()
    if n:
        print("asset budget FAILED (%d)" % n, file=sys.stderr)
        return 1
    print("asset budget PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
