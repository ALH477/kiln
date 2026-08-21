#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""test_props.py — check pm_props.py's MODELS table without Blender.

    python3 tools/blender/test_props.py [--assets <dir>]

pm_props.py is the entry point for every OBJ- and glTF-sourced PetaByte
Madness model, and its `build()` is bpy-free right up to the two kilnlib calls
that make a mesh — so the whole table can be exercised here, against the real
assets, in well under a second. The alternative for each entry is a headless
Blender launch plus a gltf_to_t3d run plus a ROM build.

── What this is really guarding ───────────────────────────────────────────
pm_props.py's own header stakes out one rule: **1 Blender unit = 1 metre,
always**, so that flake.nix's baseScale stays a single meaningful dial and a
wrong number reads as "that submarine is the size of a car" rather than as a
silent mismatch between two models that were never in the same scene. That
rule is stated in prose and enforced by nothing. A `scale` typo produces a
model that converts cleanly, ships, and is simply the wrong size — and it is
only visible once two models are on screen together, which for this game is
several screens into the intro.

So the load-bearing check here is METRES: every entry's longest dimension
after `scale`, against the size the drop's own specs say the thing is. The
expected ranges below are deliberately wide (they are sanity bounds, not a
golden test) but they are tight enough to catch a factor of ten, which is the
error that actually happens — see pm_models.h on the island being placed at
900 units when it was 12,813 across.

The rest is structural: every entry's source exists, every `color` and
`split` value is one the build path actually handles (a typo in either falls
through to a silent default rather than an error), and every "vertex" entry's
source really does carry vertex colours.

Same discipline as test_objkit.py / test_env.py / test_world.py.
"""

import os
import sys
import types
import importlib.util

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.modules.setdefault("bpy", types.ModuleType("bpy"))
sys.path.insert(0, ROOT)

import kilnlib as m  # noqa: E402
import objkit  # noqa: E402

FAIL = []
MADE = []


def check(cond, msg):
    if cond:
        print("  ok   %s" % msg)
    else:
        print("  FAIL %s" % msg)
        FAIL.append(msg)


# ── kilnlib stubs ───────────────────────────────────────────────────────
# Same approach as test_goblins.py: the builder is pure, only the two
# scene-touching calls at the end are not, so those get recorded instead of
# executed. The fake mesh carries `.data.polygons` because build() reads it to
# total the triangle count — returning something without it would make the
# stub, not the code, decide whether the test passes.
class _FakeData:
    def __init__(self, faces):
        self.polygons = list(faces)


class _FakeMesh:
    def __init__(self, name, verts, faces, colors):
        self.name = name
        self.verts = verts
        self.faces = faces
        self.colors = colors
        self.data = _FakeData(faces)
        self.modifiers = None  # every entry is decimate=None; see below


def fake_make_mesh(name, verts, faces, material, colors=None, uvs=None,
                   smooth=False):
    mesh = _FakeMesh(name, verts, faces, colors)
    MADE.append(mesh)
    return mesh


m.reset_scene = lambda: None
m.make_material = lambda name: None
m.make_mesh = fake_make_mesh
m.report = lambda max_tris=None: None
m.export_gltf = lambda *a, **k: None


# ── Expected real-world size, in metres ────────────────────────────────
# (axis, lo, hi). The axis matters: measuring the longest dimension of
# everything is what a first draft of this did, and it was wrong for the two
# entries whose stated size is not their longest dimension —
#
#   * `palms` is ONE mesh holding every plant, so its longest dimension is
#     the grove's 9.56 m footprint, not the 6 m palm the spec describes.
#   * `dank_lab` is a room; its stated figures are a footprint and a ceiling.
#
# So each entry names the axis its own spec is written in. Sources, so these
# are checkable rather than folklore (and each is confirmed against the asset
# — the parenthesised figure is what it measures today):
#
#   loach     length. docs/LOACH_spec.md gives a 2.86 m one-person boat,
#             scaled x3.5 on purpose so the 2.5 m centaur fits inside and can
#             climb out. (9.52 m)
#   dank_lab  footprint. The generator's envelope is ~10 m x 5 m. (9.86 m)
#   palms     height. pm_props.py: "already authored in metres — a 6 m palm",
#             and pm_models.h's PM_PALM_HEIGHT 388.0f is that x64. (6.06 m)
#   horner_upright, guard_*  height. People. (1.74-1.91 m)
#   drone     length. A shoulder-carried machine. (2.04 m)
#
# HEIGHT is the +Y extent, because these are measured on the raw source
# before objkit.yup_to_zup — the drop is authored Y-up.
HEIGHT, LENGTH = "height", "length"
EXPECT_METRES = {
    "loach":          (LENGTH, 7.0, 13.0),
    "dank_lab":       (LENGTH, 8.0, 14.0),
    "palms":          (HEIGHT, 4.0, 9.0),
    "horner_upright": (HEIGHT, 1.4, 2.2),
    "guard_cousin":   (HEIGHT, 1.5, 2.2),
    "guard_uncle":    (HEIGHT, 1.5, 2.2),
    "guard_whistler": (HEIGHT, 1.5, 2.2),
    "guard_auntie":   (HEIGHT, 1.5, 2.2),
    "drone":          (LENGTH, 0.5, 3.0),
}

# The lab is the one model whose ceiling height the runtime depends on
# directly — pm_lab.h's PM_LAB_REAL_Y1 and pm_lab_eye_height() both sit under
# it — so it gets a second, tighter assertion of its own.
LAB_CEILING_M = 2.50

# The values build() branches on. A `color` outside this set falls through to
# the "mtl" branch and a `split` outside it reaches group_faces as an
# unknown `by`, which quietly groups everything under one key — both are
# silent, so they are checked rather than trusted.
COLORS = {"vertex", "mtl", "island"}
SPLITS = {"material", "group", "loose"}


def main():
    assets = None
    if "--assets" in sys.argv:
        assets = sys.argv[sys.argv.index("--assets") + 1]

    # pm_props.py calls main() at module scope, so importing it runs one
    # model. Give it one that needs nothing unusual and let it run; every
    # other entry is then driven through build() directly.
    argv = ["blender", "--", "--model", "dank_lab", "--out", "/dev/null"]
    if assets:
        argv += ["--assets", assets]
    sys.argv = argv

    spec_mod = importlib.util.spec_from_file_location(
        "pm_props_under_test", os.path.join(ROOT, "pm_props.py"))
    P = importlib.util.module_from_spec(spec_mod)
    spec_mod.loader.exec_module(P)

    print("── table structure ──")
    check(len(P.MODELS) >= 8, "MODELS has entries (%d)" % len(P.MODELS))
    for name in sorted(P.MODELS):
        spec = P.MODELS[name]
        missing = [k for k in ("obj", "scale", "decimate", "color", "split")
                   if k not in spec]
        check(not missing, "%s: has every required key%s"
              % (name, "" if not missing else " (missing %s)" % missing))
        check(spec.get("color") in COLORS,
              "%s: color=%r is a value build() handles" % (name, spec.get("color")))
        check(spec.get("split") in SPLITS,
              "%s: split=%r is a value the splitter handles" % (name, spec.get("split")))
        check(isinstance(spec.get("scale"), (int, float)) and spec["scale"] > 0,
              "%s: scale is a positive number (%r)" % (name, spec.get("scale")))
        if spec.get("split") == "loose":
            check(isinstance(spec.get("loose_min_tris"), int),
                  "%s: a loose split declares loose_min_tris" % name)

    print("── sources present ──")
    for name in sorted(P.MODELS):
        path = P._source(P.MODELS[name])
        check(os.path.exists(path), "%s: %s" % (name, os.path.relpath(path, ROOT)))

    print("── every entry is in EXPECT_METRES ──")
    # Not a size check yet — a NEW model added to MODELS with no expected size
    # would otherwise skip the metres check entirely and get no coverage at
    # all, which is the failure mode of every "check the ones we know about"
    # test.
    unknown = sorted(set(P.MODELS) - set(EXPECT_METRES))
    check(not unknown,
          "no entry lacks an expected size%s"
          % ("" if not unknown else " (add %s to EXPECT_METRES)" % unknown))

    print("── metres (pm_props.py: 1 Blender unit = 1 metre) ──")
    for name in sorted(P.MODELS):
        spec = P.MODELS[name]
        if not os.path.exists(P._source(spec)) or name not in EXPECT_METRES:
            continue
        obj = P._load(spec)
        lo, hi = objkit.bbox(obj["verts"])
        d = [(hi[i] - lo[i]) * spec["scale"] for i in range(3)]
        axis, want_lo, want_hi = EXPECT_METRES[name]
        got = d[1] if axis == HEIGHT else max(d[0], d[2])
        check(want_lo <= got <= want_hi,
              "%s: %s %.2f m (expected %.1f-%.1f)"
              % (name, axis, got, want_lo, want_hi))
        if name == "dank_lab":
            check(abs(d[1] - LAB_CEILING_M) < 0.05,
                  "dank_lab: ceiling %.2f m (pm_lab.h keys the player's eye "
                  "height under it)" % d[1])

    print("── vertex-coloured sources really carry colours ──")
    # build() raises SystemExit on this, which means today it is discovered by
    # a failed Blender launch inside a Nix sandbox. It is a one-line check.
    for name in sorted(P.MODELS):
        spec = P.MODELS[name]
        if spec.get("color") != "vertex" or not os.path.exists(P._source(spec)):
            continue
        obj = P._load(spec)
        check(obj["colors"] is not None,
              "%s: source carries vertex colours" % name)

    print("── build() end to end ──")
    for name in sorted(P.MODELS):
        spec = P.MODELS[name]
        if not os.path.exists(P._source(spec)):
            continue
        del MADE[:]
        tris = P.build(spec)
        check(tris > 0, "%s: builds %d face(s)" % (name, tris))
        check(len(MADE) > 0, "%s: produces at least one named object" % name)

        # Every object's faces must index inside its OWN remapped vertex
        # array. build() remaps per group, and an off-by-one there produces a
        # mesh Blender's validate() rejects — which discards the whole
        # material silently (objkit.split_double_sided's comment).
        bad = [o.name for o in MADE
               if o.faces and max(max(f) for f in o.faces) >= len(o.verts)]
        check(not bad, "%s: no object indexes past its own vertices%s"
              % (name, "" if not bad else " (%s)" % bad))

        # One colour per vertex, per object. make_mesh raises on a mismatch,
        # but only once Blender is running.
        bad = [o.name for o in MADE
               if o.colors is not None and len(o.colors) != len(o.verts)]
        check(not bad, "%s: one colour per vertex in every object%s"
              % (name, "" if not bad else " (%s)" % bad))

        # Colours must be 4-component linear floats. kilnlib.srgb returns
        # those; a builder that passed raw 0-255 ints through instead fails
        # inside Blender as "sequences of dimension 0 should contain 4 items,
        # not 3", which is a long way from the cause.
        wrong = [o.name for o in MADE
                 if o.colors and any(len(c) != 4 for c in o.colors[:8])]
        check(not wrong, "%s: colours are 4-component%s"
              % (name, "" if not wrong else " (%s)" % wrong))

        if spec["split"] == "loose":
            names = sorted(o.name for o in MADE)
            check(any(n.startswith("palm_") for n in names),
                  "%s: the loose split names palms (%d objects)" % (name, len(names)))
            check("scatter" in names,
                  "%s: tiny fragments are merged into `scatter`, not dropped" % name)

    print()
    if FAIL:
        print("%d check(s) FAILED" % len(FAIL))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
