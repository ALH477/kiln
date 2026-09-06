#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""test_quake_map.py — the brush CSG, which everything downstream depends on.

quake_map.brush_to_faces decides whether a .map produces geometry at all, and
until this file existed it was gated only INDIRECTLY: through validate.py,
inside two round-trip checks, and only ever against a single brush. That is how
six of the seven committed .map files came to be wound inside-out — loading
perfectly on console, where kiln_map.c takes the componentwise min/max of the
plane points and is indifferent to winding, and yielding ZERO polygons here.

bpy-free by construction (quake_map imports only math/re/sys/pathlib at module
scope), so nix/checks/blender-tests.nix picks it up from its test_*.py glob with
no nix edit at all.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "schema"))
sys.path.insert(0, str(HERE.parent / "mapmaker"))
sys.path.insert(0, str(HERE.parent / "forge"))

import quake_map      # noqa: E402
import level_vocab    # noqa: E402
import mapfmt         # noqa: E402

fails = 0


def ok(cond, what):
    global fails
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        fails += 1


def planes(tris, tex="TEX"):
    return [{"p1": t[0], "p2": t[1], "p3": t[2], "texture": tex,
             "xoff": 0.0, "yoff": 0.0, "rot": 0.0, "xscale": 1.0, "yscale": 1.0}
            for t in tris]


def nfaces(br):
    # brush_to_faces returns a dict KEYED BY TEXTURE, so len() on it counts
    # textures. Getting this wrong reports a healthy cube as 1 face.
    return sum(len(v) for v in quake_map.brush_to_faces(br).values())


MN, MX = [-64.0, -32.0, -64.0], [64.0, 32.0, 64.0]

print("the canonical winding")
canon = planes(level_vocab.aabb_faces(MN, MX))
ok(nfaces(canon) == 6, f"a canonical AABB brush yields 6 faces ({nfaces(canon)})")

centre = [(MN[i] + MX[i]) / 2 for i in range(3)]
outward = 0
for tex, polys in quake_map.brush_to_faces(canon).items():
    for ring in polys:
        # Newell normal of the ring, against the vector from the centroid.
        n = [0.0, 0.0, 0.0]
        for i in range(len(ring)):
            a, b = ring[i], ring[(i + 1) % len(ring)]
            n[0] += (a[1] - b[1]) * (a[2] + b[2])
            n[1] += (a[2] - b[2]) * (a[0] + b[0])
            n[2] += (a[0] - b[0]) * (a[1] + b[1])
        mid = [sum(p[i] for p in ring) / len(ring) for i in range(3)]
        d = [mid[i] - centre[i] for i in range(3)]
        if sum(n[i] * d[i] for i in range(3)) > 0:
            outward += 1
ok(outward == 6, f"every face is wound outward from the centroid ({outward}/6)")

print("the failure that broke six committed maps")
# The three MINIMUM faces wound inward: all six normals end up pointing the
# same way per axis, so the half-space intersection collapses to a corner.
bad = planes([(t[0], t[2], t[1]) if i % 2 == 0 else t
              for i, t in enumerate(level_vocab.aabb_faces(MN, MX))])
ok(nfaces(bad) < 6,
   f"a brush with its minimum faces wound inward yields <6 faces ({nfaces(bad)})")

print("the one-unit plane-point convention still works")
# assets/quake_test.map states each plane with three points one unit apart, not
# at the box corners. Planes are planes: the CSG must not care.
x0, y0, z0 = -64, -64, -64
x1, y1, z1 = 64, 64, 64
conv = planes([
    ((x0, y0, z0), (x0, y0 + 1, z0), (x0, y0, z0 + 1)),
    ((x1, y1, z1), (x1, y1 + 1, z1), (x1 + 1, y1, z1)),
    ((x0, y0, z0), (x0, y0, z0 + 1), (x0 + 1, y0, z0)),
    ((x1, y1, z1), (x1 + 1, y1, z1), (x1, y1, z1 + 1)),
    ((x0, y0, z0), (x0 + 1, y0, z0), (x0, y0 + 1, z0)),
    ((x1, y1, z1), (x1, y1, z1 + 1), (x1, y1 + 1, z1)),
])
ok(nfaces(conv) == 6,
   f"the one-unit-apart convention still yields 6 faces ({nfaces(conv)})")

print("the parser refuses what it cannot represent")
try:
    quake_map.parse_map(
        '{\n"classname" "worldspawn"\n{\n'
        '( 0 0 0 ) ( 0 1 0 ) ( 0 0 1 ) TEX [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 1 1\n'
        '}\n}\n')
    ok(False, "a Valve 220 bracketed-UV plane is rejected")
except SystemExit:
    ok(True, "a Valve 220 bracketed-UV plane is rejected")
except Exception as e:
    ok(True, f"a Valve 220 bracketed-UV plane is rejected ({type(e).__name__})")

print("the face table has one statement")
# mapfmt and frg are the two Python emitters. Nothing held them together: the
# JS<->Python pair is covered by mapmaker-roundtrip and the Python<->C pair by
# level-vocab's probe, but Python<->Python was covered by nothing.
import frg  # noqa: E402
ok(list(mapfmt.aabb_faces(MN, MX)) == list(frg.aabb_faces(MN, MX)),
   "mapfmt.aabb_faces and frg.aabb_faces are the same table")
ok(list(mapfmt.aabb_faces(MN, MX)) == list(level_vocab.aabb_faces(MN, MX)),
   "and both are the schema's")

print()
if fails:
    print(f"test_quake_map: FAILED ({fails})")
    sys.exit(1)
print("test_quake_map passed")
