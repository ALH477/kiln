# SPDX-License-Identifier: MPL-2.0
"""test_vehicles.py — per-part winding + size check for vehicles.py.

    python3 tools/blender/test_vehicles.py     # no Blender needed

test_prims.py checks the PRIMITIVES. This checks the MODEL: every part of
the go-kart and the motorcycle, as vehicles.py actually assembles them —
because the primitives being correct does not stop a caller handing loft()
a station list that walks backwards, or slab() a polygon wound clockwise.
Both of those produce a part that is inside out with no error raised, and
both happened while these two models were being written.

Unlike test_prims.py's star-shaped check, the test here works for ANY
closed solid — a torus, a pipe that doubles back, a fender band over a
wheel — because those shapes have no single interior point to measure
against. It uses the property that actually defines orientation instead:

  * SIGNED VOLUME, by the divergence theorem. Positive means the surface
    is oriented outward. This is the real test, and it applies to every
    part regardless of shape.
  * Edge pairing, reported but NOT failed. A mesh whose directed edges all
    pair up is closed in index space; box() and cylinder() deliberately
    split their vertices per face so each face can carry its own normal and
    COLOR_0, so they never pair even though the surface is geometrically
    closed. "shared-verts" in the output just says which builder made it.

The bounding box is printed so the size claims in vehicles.py's docstring
are checked against the geometry rather than remembered.

kilnlib imports bpy at module scope but only touches it inside functions, and
vehicles.py's only bpy contact is make_mesh — so a stub module plus a
collecting make_mesh is enough to run the whole builder on the host.
"""
import sys
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.modules.setdefault("bpy", types.ModuleType("bpy"))
sys.path.insert(0, str(ROOT))

import kilnlib as m  # noqa: E402

PARTS = []


def fake_make_skinned_mesh(name, parts, armature, material):
    # vehicles.py is rigged now, so the parts arrive as (bone, verts, faces,
    # colors) through make_skinned_mesh rather than one at a time through
    # make_mesh. The bone name doubles as the label in the report, which is
    # more useful than the old per-part mesh names: it says which piece of
    # the rig every triangle belongs to.
    PARTS.extend(parts)
    return None


m.make_mesh = lambda *a, **k: None
m.make_armature = lambda name, bones: None
m.make_skinned_mesh = fake_make_skinned_mesh
m.make_action = lambda *a, **k: None
m.reset_scene = lambda: None
m.report = lambda max_tris=None: None
m.export_gltf = lambda path, animated=False: None


def analyse(verts, faces):
    # 1. edge pairing
    seen = {}
    for f in faces:
        for i in range(len(f)):
            e = (f[i], f[(i + 1) % len(f)])
            seen[e] = seen.get(e, 0) + 1
    dup = sum(1 for e, n in seen.items() if n != 1)
    unpaired = sum(1 for (a, b) in seen if (b, a) not in seen)

    # 2. signed volume (fan-triangulated)
    vol = 0.0
    for f in faces:
        for k in range(1, len(f) - 1):
            a, b, c = verts[f[0]], verts[f[k]], verts[f[k + 1]]
            vol += (a[0] * (b[1] * c[2] - b[2] * c[1])
                    - a[1] * (b[0] * c[2] - b[2] * c[0])
                    + a[2] * (b[0] * c[1] - b[1] * c[0]))
    return dup, unpaired, vol / 6.0


def check(name, verts, faces, colors):
    dup, unpaired, vol = analyse(verts, faces)
    tris = sum(len(f) - 2 for f in faces)
    problems = []
    if dup:
        problems.append(f"{dup} dup edges")
    # NOT an error: box() and cylinder() split vertices per face on purpose
    # (each face carries its own normal + COLOR_0), so their edges never pair
    # in index space even though the surface is geometrically closed. Signed
    # volume is the orientation test that applies to all of them.
    shared = "" if unpaired else " shared-verts"
    if vol <= 0:
        problems.append(f"INSIDE OUT (vol {vol:+.4f})")
    if colors is not None and len(colors) != len(verts):
        problems.append(f"{len(colors)} colors for {len(verts)} verts")
    status = (", ".join(problems) if problems
              else f"ok  vol {vol:+.4f}{shared}")
    print(f"  {name:<18} {len(verts):>4}v {tris:>4}t  {status}")
    return len(problems), len(verts), tris


def run(model):
    PARTS.clear()
    sys.argv = ["blender", "--", "--model", model, "--out", "/dev/null"]
    import importlib.util
    spec = importlib.util.spec_from_file_location("vehicles", ROOT / "vehicles.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    print(f"── {model} ──")
    bad = tv = tt = 0
    for name, verts, faces, colors in PARTS:
        b, v, t = check(name, verts, faces, colors)
        bad += b
        tv += v
        tt += t
    w = mod.WELD
    print(f"  {'WELD':<18} {w['verts_in']:>4}v->{w['verts_out']:>4}v  "
          f"{w['tris_in']:>4}t->{w['tris_out']:>4}t  dropped {w['dropped']}")
    off_grid = sum(1 for _, vs, _, _ in PARTS for v in vs for c in v
                   if abs(c / m.N64_GRID - round(c / m.N64_GRID)) > 1e-6)
    if off_grid:
        print(f"  {off_grid} coordinates OFF the 1/64 grid")
        bad += 1

    allv = [v for _, vs, _, _ in PARTS for v in vs]
    lo = [min(v[i] for v in allv) for i in range(3)]
    hi = [max(v[i] for v in allv) for i in range(3)]
    print(f"  {'TOTAL':<18} {tv:>4}v {tt:>4}t")
    print(f"  bbox  x[{lo[0]:+.2f},{hi[0]:+.2f}] "
          f"y[{lo[1]:+.2f},{hi[1]:+.2f}] z[{lo[2]:+.2f},{hi[2]:+.2f}]")
    return bad


def main():
    fail = run("gokart") + run("bike")
    print("test_vehicles: FAILED" if fail
          else "test_vehicles: all parts closed and wound outward")
    return 1 if fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
