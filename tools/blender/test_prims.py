# SPDX-License-Identifier: MIT
"""test_prims.py — winding checks for kilnlib's primitive builders.

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

kilnlib imports bpy at module scope but only touches it inside functions, so a
stub module is enough to import the pure builders.
"""

import math
import sys
import types
from pathlib import Path

sys.modules.setdefault("bpy", types.ModuleType("bpy"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import kilnlib as m  # noqa: E402


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


def check_outward_local(label, verts, faces, path):
    """Outward check for a solid that is star-shaped only LOCALLY.

    check_outward compares every face against one interior point, which is
    exactly right for a convex-ish solid and wrong for anything that curls
    back on itself: a helix's far end is nowhere near its middle, so a
    perfectly wound face there dots negative against a single global centre
    and reads as a false failure. Here each face is judged against the
    nearest point on the swept centreline instead — the reference a swept
    tube actually has.
    """
    def nearest(c):
        return min(path, key=lambda p: sum((a - b) ** 2 for a, b in zip(c, p)))

    bad = []
    for i, f in enumerate(faces):
        c = centroid(verts, f)
        o = nearest(c)
        if sum(a * b for a, b in zip(area_normal(verts, f),
                                     (x - y for x, y in zip(c, o)))) <= 0:
            bad.append(i)
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

    # ── sweep ─────────────────────────────────────────────────────────────
    # A square section, CCW in (u,v). Reused across all three paths below so
    # a winding failure can only be the frame, never the section.
    sq = [(0.25, -0.25), (0.25, 0.25), (-0.25, 0.25), (-0.25, -0.25)]

    # Straight, the degenerate case: sweep must agree with loft here.
    verts, faces = m.sweep([(0, -1, 0), (0, 0, 0), (0, 1, 0)], sq)
    fail += check_outward("sweep straight (+Y)", verts, faces)

    # An arc. Not star-shaped about the origin, so the interior reference
    # point is the arc's own midpoint — check_outward's `inside` exists for
    # exactly this.
    # Curved paths are checked UNCAPPED, for two reasons. The one that shows
    # up in the numbers: a cap's centroid sits ON its end path point, so the
    # local reference has no radial offset to compare against and every cap
    # reads as a false failure. The one that makes it sound: loft() decides
    # cap winding from the ring order and which end it is, not from anything
    # the path does, so the straight case above already covers it — bending
    # the path cannot break a cap without also breaking a side.
    path = m.arc_path((0, 0, 0), 1.0, 0, 90, 5, plane="yz")
    verts, faces = m.sweep(path, sq, cap_start=False, cap_end=False)
    fail += check_outward_local("sweep 90deg arc (YZ)", verts, faces, path)

    # A helix turns the frame in all three axes at once — the case where a
    # hand-rolled frame that happens to work on a planar arc comes apart.
    helix = [(math.cos(i * 0.5), math.sin(i * 0.5), i * 0.35) for i in range(6)]
    verts, faces = m.sweep(helix, sq, cap_start=False, cap_end=False)
    fail += check_outward_local("sweep helix", verts, faces, helix)

    # rotated() must be a rotation, not a reflection: a box laid on its side
    # stays outward-facing with its point list untouched.
    verts, faces = m.box(0, 0, 0, 1.0, 0.4, 0.4)
    verts = m.rotated(verts, (0, 1, 0), 90)
    fail += check_outward("box, rotated 90 about Y", verts, faces)
    verts = m.translated(verts, dx=3.0)
    fail += check_outward("box, rotated + translated", verts, faces,
                          inside=(3.0, 0.0, 0.0))

    # A cylinder laid on its side — the wheel construction every vehicle in
    # tools/blender/vehicles.py uses.
    verts, faces = m.cylinder(0.5, 0.3, 8, cz=-0.15)
    verts = m.rotated(verts, (0, 1, 0), 90)
    fail += check_outward("cylinder, axle along X", verts, faces)

    # segment(): a limb between two joints. Checked on all three interesting
    # directions — a diagonal (the normal case), straight up (where the
    # rotation axis vanishes) and straight DOWN (where the naive fix is a
    # mirror, which would turn the limb inside out without changing its
    # silhouette at all).
    verts, faces = m.segment((0, 0, 0), (0.6, 0.3, -0.9), 0.2, 0.12)
    fail += check_outward("segment, diagonal", verts, faces,
                          inside=(0.3, 0.15, -0.45))
    verts, faces = m.segment((0, 0, 0), (0, 0, 1.0), 0.2, 0.2)
    fail += check_outward("segment, +Z (axis vanishes)", verts, faces,
                          inside=(0, 0, 0.5))
    verts, faces = m.segment((0, 0, 0), (0, 0, -1.0), 0.2, 0.1)
    fail += check_outward("segment, -Z (180 case)", verts, faces,
                          inside=(0, 0, -0.5))

    fail += check_rejects("segment: coincident ends",
                          lambda: m.segment((0, 0, 0), (0, 0, 0), 1, 1))

    # ── quantise + weld ───────────────────────────────────────────────────
    # A cube whose 24 split corners carry SIX distinct face colours must weld
    # to 24, not to 8: the colour is half the merge key, and a position-only
    # weld would average the faces together. This is the check that stops a
    # future "optimisation" from quietly flattening every box in the repo.
    verts, faces = m.box(0, 0, 0, 1, 1, 1)
    cols = m.expand_colors("t", [(1, 0, 0, 1), (0, 1, 0, 1), (0, 0, 1, 1),
                                 (1, 1, 0, 1), (0, 1, 1, 1), (1, 0, 1, 1)],
                           verts, faces)
    _v, _f, _c, st = m.weld(verts, faces, cols)
    ok = st["verts_out"] == 24 and st["faces_out"] == 6
    print(f"  {'weld: 6-colour cube':<28} "
          f"{st['verts_in']}v->{st['verts_out']}v  "
          f"{st['faces_in']}f->{st['faces_out']}f  "
          f"{'ok' if ok else 'FAIL (colours merged)'}")
    fail += 0 if ok else 1

    # The same cube with ONE colour everywhere shares its corners: 24 -> 8.
    _v, _f, _c, st = m.weld(verts, faces, [(1, 1, 1, 1)] * len(verts))
    ok = st["verts_out"] == 8 and st["faces_out"] == 6
    print(f"  {'weld: 1-colour cube':<28} "
          f"{st['verts_in']}v->{st['verts_out']}v  "
          f"{st['faces_in']}f->{st['faces_out']}f  "
          f"{'ok' if ok else 'FAIL'}")
    fail += 0 if ok else 1

    # Sub-grid detail is what quantisation destroys. A cone whose tip ring
    # sits 1/500 of a unit across collapses to a point: the tip triangles
    # must go, and the ones below them must survive.
    tip = [(0.001 * math.cos(i), 1.0, 0.001 * math.sin(i)) for i in range(8)]
    mid = [(0.5 * math.cos(i), 0.0, 0.5 * math.sin(i)) for i in range(8)]
    verts, faces = m.loft([mid, tip], cap_start=False, cap_end=False)
    _v, _f, _c, st = m.weld(verts, faces)
    ok = st["tris_out"] < st["tris_in"] and st["tris_out"] > 0
    print(f"  {'weld: sub-grid cone tip':<28} "
          f"{st['tris_in']}t->{st['tris_out']}t  dropped {st['dropped']}  "
          f"{'ok' if ok else 'FAIL'}")
    fail += 0 if ok else 1

    # Welding must not move anything off the grid, and must be idempotent.
    verts, faces = m.segment((0, 0, 0), (0.37, 0.11, -0.83), 0.19, 0.07, 6)
    v1, f1, _c, _s = m.weld(verts, faces)
    v2, f2, _c, s2 = m.weld(v1, f1)
    ok = (len(v1) == len(v2) and len(f1) == len(f2)
          and all(abs(c / m.N64_GRID - round(c / m.N64_GRID)) < 1e-9
                  for v in v1 for c in v))
    print(f"  {'weld: on-grid + idempotent':<28} "
          f"{len(v1)}v {len(f1)}f  {'ok' if ok else 'FAIL'}")
    fail += 0 if ok else 1
    fail += check_outward("welded segment still solid", v1, f1,
                          inside=(0.185, 0.055, -0.415))

    # ease() must be pinned at both ends whatever the mode, or an action
    # built from it will not return to its own start pose and every loop
    # will visibly pop.
    bad_ease = [mo for mo in ("in", "out", "inout", "over")
                if abs(m.ease(0.0, mo)) > 1e-9 or abs(m.ease(1.0, mo) - 1) > 1e-9]
    print(f"  {'ease: endpoints pinned':<28} "
          f"{'ok' if not bad_ease else 'FAIL ' + str(bad_ease)}")
    fail += len(bad_ease)

    fail += check_rejects("sweep: one path point",
                          lambda: m.sweep([(0, 0, 0)], sq))
    fail += check_rejects("sweep: path along up",
                          lambda: m.sweep([(0, 0, 0), (0, 0, 1)], sq))
    fail += check_rejects("rotated: zero axis",
                          lambda: m.rotated([(1, 0, 0)], (0, 0, 0), 90))

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
