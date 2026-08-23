# SPDX-License-Identifier: MIT
"""test_goblins.py — per-part winding + size check for goblin.py's cast.

    python3 tools/blender/test_goblins.py     # no Blender needed

The same check test_vehicles.py runs, over every character goblin.py builds.
Each part's SIGNED VOLUME must be positive — the property that says a closed
surface is oriented outward — because an inside-out chunk converts cleanly,
raises nothing anywhere in the pipeline, and then silently vanishes under the
RDP's back-face culling.

It earns its keep twice over here. The body masses are lofted up +Z, and
loft()'s winding rule works out to the opposite handedness for a +Z sweep
than for the +Y sweeps vehicles.py uses (see _stack()'s comment) — a rule
that is easy to state and easy to get backwards. And there are five
characters sharing one builder, so a mistake in a per-character branch shows
up on one of them and not the others, which is exactly the kind of thing a
single spot-check misses.

Bounding boxes are printed so the proportions each character is supposed to
have can be read off rather than assumed — Moss the widest body, Sparky the
narrowest. Note the WIDTH and HEIGHT columns include ears and headgear, so
Glimmer measures widest of all (her ears) and Dank tallest (his hat) even
though every character stands on the same rig at the same bone height.
"""
import sys
import types
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.modules.setdefault("bpy", types.ModuleType("bpy"))
sys.path.insert(0, str(ROOT))

import kilnlib as m  # noqa: E402

PARTS = []


def fake_make_skinned_mesh(name, parts, armature, material):
    PARTS.extend(parts)
    return None


m.make_armature = lambda name, bones: None
m.make_skinned_mesh = fake_make_skinned_mesh
m.make_action = lambda *a, **k: None
m.reset_scene = lambda: None
m.report = lambda max_tris=None: None
m.export_gltf = lambda *a, **k: None
# The real make_mesh needs bpy; nothing in the skinned path calls it, but the
# extras builders go through the parts list, so this is only a guard.
m.make_mesh = lambda *a, **k: None


def analyse(verts, faces):
    seen = {}
    for f in faces:
        for i in range(len(f)):
            e = (f[i], f[(i + 1) % len(f)])
            seen[e] = seen.get(e, 0) + 1
    dup = sum(1 for e, n in seen.items() if n != 1)
    unpaired = sum(1 for (a, b) in seen if (b, a) not in seen)

    vol = 0.0
    for f in faces:
        for k in range(1, len(f) - 1):
            a, b, c = verts[f[0]], verts[f[k]], verts[f[k + 1]]
            vol += (a[0] * (b[1] * c[2] - b[2] * c[1])
                    - a[1] * (b[0] * c[2] - b[2] * c[0])
                    + a[2] * (b[0] * c[1] - b[1] * c[0]))
    return dup, unpaired, vol / 6.0


def run(model):
    PARTS.clear()
    sys.argv = ["blender", "--", "--model", model, "--out", "/dev/null"]
    spec = importlib.util.spec_from_file_location(f"gob_{model}",
                                                  ROOT / "goblin.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    print(f"── {model} ──")
    bad = tv = tt = 0
    per_bone = {}
    for bone, verts, faces, colors in PARTS:
        dup, unpaired, vol = analyse(verts, faces)
        tris = sum(len(f) - 2 for f in faces)
        problems = []
        if dup:
            problems.append(f"{dup} dup edges")
        if vol <= 0:
            problems.append(f"INSIDE OUT (vol {vol:+.4f})")
        if colors is not None and len(colors) != len(verts):
            problems.append(f"{len(colors)} colors for {len(verts)} verts")
        if problems:
            print(f"  {bone:<12} {len(verts):>4}v {tris:>4}t  "
                  f"{', '.join(problems)}")
            bad += len(problems)
        tv += len(verts)
        tt += tris
        per_bone[bone] = per_bone.get(bone, 0) + tris

    w = mod.WELD
    print(f"  {'WELD':<12} {w['verts_in']:>4}v->{w['verts_out']:>4}v  "
          f"{w['tris_in']:>4}t->{w['tris_out']:>4}t  "
          f"dropped {w['dropped']}")

    # Everything must sit exactly on the console's vertex grid after the
    # weld — that is the whole point of quantising at author time, and it is
    # the property a later "just nudge it a bit" edit would silently break.
    off_grid = sum(1 for _, vs, _, _ in PARTS for v in vs for c in v
                   if abs(c / m.N64_GRID - round(c / m.N64_GRID)) > 1e-6)
    if off_grid:
        print(f"  {off_grid} coordinates OFF the 1/64 grid")
        bad += 1

    allv = [v for _, vs, _, _ in PARTS for v in vs]
    lo = [min(v[i] for v in allv) for i in range(3)]
    hi = [max(v[i] for v in allv) for i in range(3)]
    print(f"  {'TOTAL':<12} {tv:>4}v {tt:>4}t   "
          f"w {hi[0] - lo[0]:.2f}  d {hi[1] - lo[1]:.2f}  "
          f"h {hi[2] - lo[2]:.2f}")
    return bad, tt


def main():
    fail = 0
    for name in ("goblin", "dank", "sparky", "moss", "glimmer"):
        bad, tris = run(name)
        fail += bad
        if tris > 900:
            print(f"  {name}: {tris} tris over the 900 ceiling")
            fail += 1
    print("test_goblins: FAILED" if fail
          else "test_goblins: all parts closed and wound outward")
    return 1 if fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
