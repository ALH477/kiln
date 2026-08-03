# SPDX-License-Identifier: MPL-2.0
"""test_prims.py — winding checks for m64lib's primitive builders.

    python3 tools/blender/test_prims.py        # no Blender needed

Face winding is the one property of a generated mesh that a headless build
cannot notice and a screenshot barely can: an inside-out solid does not fail
to convert, does not warn, and on hardware just quietly disappears under the
RDP's back-face culling (or, worse, renders only its far side, which reads as
a Z-buffer bug). So it is checked here, in plain Python, before Blender is
ever started — the same discipline tools/blender/quake_map.py's parser half
follows, and for the same reason.

The check itself: for a closed solid that is star-shaped about an interior
point, EVERY face normal must point away from that point. Normals come from
Newell's method rather than the first triangle's cross product, so a
non-planar quad is judged by its whole outline.

m64lib imports bpy at module scope but only touches it inside functions, so a
stub module is enough to import the pure builders.
"""

import math
import sys
import types
from pathlib import Path

sys.modules.setdefault("bpy", types.ModuleType("bpy"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import m64lib as m  # noqa: E402


def area_normal(verts, face):
    nx = ny = nz = 0.0
    for i in range(len(face)):
        x0, y0, z0 = verts[face[i]]
        x1, y1, z1 = verts[face[(i + 1) % len(face)]]
        nx += (y0 - y1) * (z0 + z1)
        ny += (z0 - z1) * (x0 + x1)
        nz += (x0 - x1) * (y0 + y1)
    return (nx, ny, nz)


def centroid(verts, face):
    return tuple(sum(c) / len(face) for c in zip(*(verts[i] for i in face)))


def check_outward(label, verts, faces, inside=(0.0, 0.0, 0.0)):
    bad = [i for i, f in enumerate(faces)
           if sum(a * b for a, b in
                  zip(area_normal(verts, f),
                      (c - o for c, o in zip(centroid(verts, f), inside)))) <= 0]
    status = f"{len(bad)} INWARD {bad[:6]}" if bad else "ok"
    print(f"  {label:<28} {len(verts):>3} verts {len(faces):>3} faces  {status}")
    return len(bad)


def check_rejects(label, call):
    try:
        call()
    except SystemExit as exc:
        print(f"  {label:<28} rejected: {str(exc)[:48]}")
        return 0
    print(f"  {label:<28} NO ERROR RAISED")
    return 1


def ring(radius, y, n=8):
    return [(radius * math.cos(2 * math.pi * i / n), y,
             radius * math.sin(2 * math.pi * i / n)) for i in range(n)]


def main():
    fail = 0

    # A tapering tube swept along +Y: the loft case interceptor.py leans on.
    verts, faces = m.loft([ring(1.0, -1.0), ring(1.0, 0.0), ring(0.6, 1.0)])
    fail += check_outward("loft tube (+Y sweep)", verts, faces)

    # Uncapped, so the sides are judged alone — a cap error cannot mask a
    # side error or the other way round.
    verts, faces = m.loft([ring(1.0, -1.0), ring(0.8, 1.0)],
                          cap_start=False, cap_end=False)
    fail += check_outward("loft open tube", verts, faces)

    verts, faces = m.slab([(-1, -1, 0, 0.2), (1, -1, 0, 0.2),
                           (1, 1, 0, 0.05), (-1, 1, 0, 0.05)])
    fail += check_outward("slab, tapered", verts, faces)

    # mirror_x's entire job: negating x alone would leave this one inverted.
    wing = [(0.5, -1.0, 0, 0.18), (2.0, -0.9, 0, 0.07),
            (2.0, 0.4, 0, 0.07), (0.5, 1.0, 0, 0.18)]
    verts, faces = m.slab(m.mirror_x(wing))
    fail += check_outward("slab, mirrored", verts, faces, inside=(-1.2, 0.0, 0.0))

    fail += check_rejects("loft: one section", lambda: m.loft([ring(1, 0)]))
    fail += check_rejects("loft: ragged sections",
                          lambda: m.loft([ring(1, 0), ring(1, 1, n=6)]))
    fail += check_rejects("loft: 2-point section",
                          lambda: m.loft([[(0, 0, 0), (1, 0, 0)]] * 2))
    fail += check_rejects("slab: two points",
                          lambda: m.slab([(0, 0, 0, 1), (1, 0, 0, 1)]))

    print("test_prims: FAILED" if fail else "test_prims: all checks pass")
    return 1 if fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
