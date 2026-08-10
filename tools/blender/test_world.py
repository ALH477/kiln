#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""test_world.py — check pm_world.py's geometry without Blender.

    python3 tools/blender/test_world.py

These models exist to satisfy the RUNTIME, so the checks are about the
properties the runtime depends on rather than about how they look: walkable
ground actually flat (or `m64_clip`'s AABBs cannot match it), gates on their
bearings (or a room transition fires in the wrong place), triangle counts
inside budget (or the console cannot draw the hub at 60 fps).

Same approach as test_env.py; see pm_world.py's header.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pm_world as W  # noqa: E402

FAIL = []


def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        FAIL.append(msg)


def tris(faces):
    return sum(len(f) - 2 for f in faces)


def main():
    print("── island terrain ──")
    v, f, c = W.build_island_terrain()
    check(len(v) == len(c), "one colour per vertex (%d)" % len(v))
    check(max(i for face in f for i in face) < len(v),
          "no face indexes past the vertex array")
    check(all(len(x) in (3, 4) for x in f), "faces are tris or quads")
    n = tris(f)
    # Raised from 1,400 deliberately when the terrain density went up: the
    # limit on this hardware is fill rate, not triangles, and the island
    # covers the same screen area at any density.
    check(n < 3200, "terrain inside budget (%d tris)" % n)

    # Regression: the ring radii must ascend and finish exactly at R_WATER.
    # They were once a literal list that stayed put through a rescale, and
    # three consecutive pairs ended up running backwards — the quad bands
    # folded back through each other and two rings sat past the waterline.
    r = W.terrain_rings()
    check(all(b > a for a, b in zip(r, r[1:])),
          "terrain rings ascend (%d rings)" % len(r))
    check(abs(r[-1] - W.R_WATER) < 1e-9,
          "the outermost ring is exactly R_WATER (%.2f)" % r[-1])
    check(W.SECTORS % W.GATE_COUNT == 0,
          "sectors divide evenly among the gates (%d / %d)"
          % (W.SECTORS, W.GATE_COUNT))
    check(W.PATH_HALF_DEG > (360.0 / W.SECTORS) * 0.5,
          "a path is wider than half a sector (%.1f deg > %.1f)"
          % (W.PATH_HALF_DEG, (360.0 / W.SECTORS) * 0.5))

    # The whole point: the field is FLAT, so one brush covers it.
    field = [p for p in v if math.hypot(p[0], p[1]) <= W.R_FIELD - 0.1]
    zs = {round(p[2], 4) for p in field}
    check(zs == {W.FIELD_Z},
          "the central field is exactly one height (%s)" % sorted(zs))
    check(len(field) > 20, "the field actually has vertices in it (%d)" % len(field))

    # Paths must be flat too, and at the same height, or a walk from the
    # field to a gate steps up and down.
    for b in W.gate_bearings():
        zs = set()
        # Sample radii DERIVED from the layout, not typed. An earlier version
        # hardcoded 62 and 70 and started failing the moment R_GATE moved —
        # a test that restates a dimension drifts from it exactly like the
        # camera keys did.
        span = W.R_GATE - W.R_FIELD
        for r in (W.R_FIELD + span * 0.1, W.R_FIELD + span * 0.5,
                  W.R_FIELD + span * 0.9, W.R_GATE):
            z, kind = W.island_height(r, b)
            zs.add((round(z, 4), kind))
        heights = {z for z, _ in zs}
        kinds = {k for _, k in zs}
        check(heights == {W.GATE_Z},
              "path on bearing %.0f is level (%s)" % (b, sorted(heights)))
        check(kinds == {"path"},
              "path on bearing %.0f is classed as path (%s)" % (b, sorted(kinds)))

    # And the ground BETWEEN paths must rise, or there are no paths, just a
    # disc with lines painted on it.
    mid = (W.gate_bearings()[0] + W.gate_bearings()[1]) * 0.5
    z_mid, kind_mid = W.island_height(
        W.R_FIELD + (W.R_GATE - W.R_FIELD) * 0.5, mid)
    check(z_mid > W.FIELD_Z + 5.0,
          "ground between paths rises to %.1f m (kind %s)" % (z_mid, kind_mid))
    check(kind_mid == "hill", "between-path ground is classed as hill")

    # The rim must CROSS sea level and finish below it. Ending exactly at
    # z = 0 puts the island's outer ring coplanar with the sea plane over
    # the whole overlap, which z-fights and shimmers as the camera moves —
    # the waterline has to be an intersection, not a shared surface.
    z_edge, _ = W.island_height(W.R_WATER, 0.0)
    check(z_edge < -1.0, "the rim finishes below sea level (%.2f m)" % z_edge)
    z_shore, _ = W.island_height(W.R_SHORE, 0.0)
    check(z_shore > 0.0, "the beach is still above water at R_SHORE (%.2f m)"
          % z_shore)
    # ...which means a shoreline exists somewhere between the two.
    crossings = 0
    prev = z_shore
    steps = 24
    for i in range(1, steps + 1):
        r = W.R_SHORE + (W.R_WATER - W.R_SHORE) * i / steps
        z, _ = W.island_height(r, 0.0)
        if (prev > 0.0) != (z > 0.0):
            crossings += 1
        prev = z
    check(crossings == 1, "the rim crosses the waterline exactly once (%d)"
          % crossings)

    # The coastline must not be a circle, or it reads as a machine part.
    wob = [W.coast_wobble(t * 10.0) for t in range(36)]
    spread = max(wob) - min(wob)
    check(spread > 0.15, "the coastline wanders (%.0f%% radius variation)"
          % (spread * 100))
    # And the six ridges must differ, for the same reason.
    scales = [W.ridge_scale(b) for b in W.gate_bearings()]
    check(max(scales) - min(scales) > 0.15,
          "the ridges are not all the same height (%.2f..%.2f)"
          % (min(scales), max(scales)))

    # The MEASURED radius (wobble included) is what the camera is derived
    # from, so that is what gets checked — R_WATER alone understates it.
    meas = W.measure()
    check(4500 < meas["island_radius"] < 9000,
          "island radius is in the range the flyover orbit assumes (%.0f)"
          % meas["island_radius"])
    check(meas["island_top"] > meas["field_y"],
          "the tower is above the field (%.0f > %.0f)"
          % (meas["island_top"], meas["field_y"]))

    print("── gates ──")
    gv, gf, gc = W.build_gates()
    check(len(gv) == len(gc), "one colour per vertex (%d)" % len(gv))
    check(tris(gf) < 500, "gates inside budget (%d tris)" % tris(gf))
    check(len(W.gate_bearings()) == 6, "six gates")
    check(len(set(W.gate_bearings())) == 6, "six DISTINCT bearings")
    # Every gate sits on the gate ring, near its bearing.
    for b in W.gate_bearings():
        want = (math.cos(math.radians(b)) * W.R_GATE,
                math.sin(math.radians(b)) * W.R_GATE)
        near = min(math.hypot(p[0] - want[0], p[1] - want[1]) for p in gv)
        check(near < 6.0, "a gate stands on bearing %.0f (%.1f m away)" % (b, near))

    print("── landmark ──")
    lv, lf, lc = W.build_landmark()
    top = max(p[2] for p in lv)
    check(top > W.HILL_Z, "the tower clears the ridges (%.1f m > %.1f m)"
          % (top, W.HILL_Z))
    check(tris(lf) < 100, "tower inside budget (%d tris)" % tris(lf))

    print("── lab ──")
    bv, bf, bc = W.build_lab()
    check(len(bv) == len(bc), "one colour per vertex (%d)" % len(bv))
    check(max(i for face in bf for i in face) < len(bv),
          "no face indexes past the vertex array")
    check(tris(bf) < 600, "lab inside budget (%d tris)" % tris(bf))
    # The walkable deck must be ONE plane at z = 0, which is the whole
    # reason to generate this model rather than import one: pm_lab.c's
    # collision brushes put the floor at a single Y and every step the
    # player takes is tested against it.
    #
    # Checked as "how many vertices sit exactly on z=0", not as "the lowest
    # vertex is the floor" — the moon pool is a RECESS and is legitimately
    # below the deck, so a min() over the whole mesh finds the pool and
    # reports a fault that is not one.
    deck = [p for p in bv if abs(p[2]) < 1e-6]
    check(len(deck) >= 12,
          "the walkable deck is a single plane at z=0 (%d vertices on it)"
          % len(deck))
    pool_bottom = min(p[2] for p in bv)
    check(pool_bottom < -W.WALL_T,
          "the moon pool is recessed below the deck (%.2f m)" % pool_bottom)
    ceil_z = max(p[2] for p in bv)
    check(ceil_z > W.LAB_H, "there is a ceiling above head height (%.2f m)" % ceil_z)
    # Extents, so pm_lab.c's brushes can be written from these numbers.
    xs = [p[0] for p in bv]
    ys = [p[1] for p in bv]
    print("       lab extents: X %.1f..%.1f  Y %.1f..%.1f  (metres)"
          % (min(xs), max(xs), min(ys), max(ys)))
    check(max(xs) - min(xs) < 12.0, "lab is not wider than its brushes assume")

    print()
    if FAIL:
        print("%d check(s) FAILED" % len(FAIL))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
