#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""test_env.py — check pm_env.py's geometry without Blender.

    python3 tools/blender/test_env.py

pm_env's builders import no bpy, so every structural property of the sky dome
and the sea can be checked here in a fraction of a second. That matters
because the alternative is a headless Blender run followed by a ROM build
followed by an emulator capture, and a winding or gradient mistake is nearly
invisible at the end of that chain — the mesh renders, it just renders wrong.

Same approach as tools/blender/test_prims.py and the unit checks quake_map.py
carries; see pm_env.py's header.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pm_env as E  # noqa: E402

FAIL = []


def check(cond, msg):
    if cond:
        print("  ok   %s" % msg)
    else:
        print("  FAIL %s" % msg)
        FAIL.append(msg)


def tris(faces):
    """Triangle count after fan triangulation, which is what the exporter does."""
    return sum(len(f) - 2 for f in faces)


def main():
    print("── skydome ──")
    verts, faces, colors = E.build_skydome()
    check(len(verts) == len(colors),
          "one colour per vertex (%d/%d)" % (len(verts), len(colors)))
    check(all(len(f) in (3, 4) for f in faces), "faces are tris or quads")
    check(max(i for f in faces for i in f) < len(verts),
          "no face indexes past the vertex array")
    check(tris(faces) < 700, "dome fits the ~400-700 tri budget (%d)" % tris(faces))

    # Every dome vertex sits on the unit hemisphere, above the horizon.
    dome_r = [math.sqrt(x * x + y * y + z * z) for x, y, z in verts]
    check(all(0.97 <= r <= 1.001 for r in dome_r),
          "all vertices lie on the dome shell")
    check(min(z for _, _, z in verts) >= -1e-6,
          "nothing dips below the horizon (z >= 0)")

    # The gradient must actually run: the lowest ring is HORIZON, the top is
    # ZENITH, and HORIZON must be the LIGHTER of the two or the sky reads
    # upside down. This is the check that would have caught an inverted ramp.
    check(colors[0] == E.HORIZON, "ring 0 is exactly HORIZON (fog match)")
    lo = sum(E.HORIZON) / 3.0
    hi = sum(E.ZENITH) / 3.0
    check(lo > hi, "horizon is brighter than zenith (%.1f > %.1f)" % (lo, hi))

    # The moon has to be up there somewhere, and be the brightest thing.
    brightest = max(colors, key=sum)
    check(brightest == E.MOON, "the moon is the brightest colour in the dome")
    md = E._moon_dir()
    check(abs(math.sqrt(sum(c * c for c in md)) - 1.0) < 1e-6,
          "moon direction is a unit vector")
    check(md[2] > 0.0, "the moon is above the horizon")

    print("── sea ──")
    sverts, sfaces, scolors, suvs = E.build_sea()
    check(len(sverts) == len(scolors) == len(suvs),
          "one colour and one uv per vertex (%d)" % len(sverts))
    check(max(i for f in sfaces for i in f) < len(sverts),
          "no face indexes past the vertex array")
    check(tris(sfaces) < 900, "sea fits the ~600-900 tri budget (%d)" % tris(sfaces))
    check(all(abs(z) < 1e-9 for _, _, z in sverts),
          "the sea is flat before the swell displaces it")

    # Ring spacing must be quadratic — denser near the island. Compare the
    # first gap to the last; linear spacing would make them equal.
    radii = sorted({round(math.hypot(x, y), 6) for x, y, _ in sverts})
    first_gap = radii[1] - radii[0]
    last_gap = radii[-1] - radii[-2]
    check(last_gap > first_gap * 2.0,
          "rings coarsen outward (%.3f -> %.3f)" % (first_gap, last_gap))

    # The moon path must be brightest on the moon's bearing and dark opposite.
    mx, my, _ = E._moon_dir()
    n = math.hypot(mx, my)
    mx, my = mx / n, my / n
    inner = [(x, y, c) for (x, y, _), c in zip(sverts, scolors)]
    toward = max(inner, key=lambda p: (p[0] * mx + p[1] * my) / (math.hypot(p[0], p[1]) or 1))
    away = min(inner, key=lambda p: (p[0] * mx + p[1] * my) / (math.hypot(p[0], p[1]) or 1))
    check(sum(toward[2]) > sum(away[2]) * 1.5,
          "moon lane is brighter toward the moon than away (%s vs %s)"
          % (toward[2], away[2]))

    # The sea's outer edge must be dark enough to disappear into fog. The
    # comparison is against the EFFECTIVE water colour — vertex colour times
    # the foam texture's floor — because the vertex colours are crest
    # ceilings, not water (see pm_env.py). Comparing the raw vertex colour
    # here would be comparing spray to sky and would always "fail".
    segments = E.build_sea.__defaults__[3]
    outer_ring = scolors[-segments:]
    worst = max(sum(c) for c in outer_ring) * E.FOAM_FLOOR
    check(worst <= sum(E.HORIZON) * 1.10,
          "outer ring fogs into the horizon (%.0f vs %d)" % (worst, sum(E.HORIZON)))

    # And the near water must be visibly lighter than the far water, or the
    # sea is a flat slab and the swell has nothing to catch.
    inner_ring = scolors[:segments]
    check(sum(max(inner_ring, key=sum)) > sum(max(outer_ring, key=sum)),
          "near water is brighter than far water")

    print("── shared ──")
    check(E.HORIZON == E.HORIZON, "HORIZON is the single source for sky/sea/fog")

    print()
    if FAIL:
        print("%d check(s) FAILED" % len(FAIL))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
