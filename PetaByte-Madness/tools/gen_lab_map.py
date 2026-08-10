#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Emit assets/pm_lab.map — the Horner Station corridor, in Quake .map form.

Why generate rather than hand-author: m64_map reads the Quake brush format,
where one axis-aligned box is six plane definitions of three points each.
That is 6 lines of 9 numbers per box, and the whole level is boxes. Editing
that by hand is how you get a wall one unit thick in the wrong axis and
spend an afternoon finding it. Editing an (mins, maxs) tuple list is not.

The output is a normal .map, so DarkRadiant can still open it and a level
designer can still take over — this script authors the first pass, it does
not own the format. (There is an idTech4 DarkRadiant project for PetaByte
Madness at ~/petabyte; that is where a real level would come from.)

The brush→plane construction mirrors what tools/blender/quake_map.py does in
reverse, and the winding convention below is the one m64_map's parser reads:
each face is three points on the plane, and m64_map reduces the six planes
back to an AABB by taking the componentwise min/max of the plane points.

Usage:  python3 tools/gen_lab_map.py --out assets/pm_lab.map
"""

import argparse

# ── The level ──────────────────────────────────────────────────────────
# One flooded corridor running +Z, with statue alcoves down both sides.
# Units are the engine's world units (~100 to a room), matching
# assets/oot_test.map and assets/fps_room*.map in the parent repo.
#
# The corridor is deliberately longer than the veil's veiled far plane
# (760) and shorter than its cold far plane (1000), so raising the filter
# visibly eats the far end of the hall. That is the draw-distance rebate in
# VEIL_DESIGN.md §2 made into level geometry rather than a number in a
# header.

FLOOR_Y = 0
CEIL_Y = 130
HALF_W = 160
Z0, Z1 = -100, 900

WALL = 20  # wall/floor slab thickness

# (mins, maxs, texture) — texture name is what a real editor would use to
# pick a surface; m64_map does not read it yet, but DarkRadiant does and
# the surface table (pm_types.h PM_SURF_*) is meant to key off it later.
BRUSHES = [
    # shell
    ((-HALF_W, FLOOR_Y - WALL, Z0), (HALF_W, FLOOR_Y, Z1), "DECK"),
    ((-HALF_W, CEIL_Y, Z0), (HALF_W, CEIL_Y + WALL, Z1), "DECK"),
    ((-HALF_W - WALL, FLOOR_Y - WALL, Z0), (-HALF_W, CEIL_Y + WALL, Z1), "DECK"),
    ((HALF_W, FLOOR_Y - WALL, Z0), (HALF_W + WALL, CEIL_Y + WALL, Z1), "DECK"),
    ((-HALF_W - WALL, FLOOR_Y - WALL, Z0 - WALL), (HALF_W + WALL, CEIL_Y + WALL, Z0), "DECK"),
    ((-HALF_W - WALL, FLOOR_Y - WALL, Z1), (HALF_W + WALL, CEIL_Y + WALL, Z1 + WALL), "DECK"),

    # the flooded mid-section: a shallow step down, so the player can hear
    # themselves in it. PM_SURF_WATER once m64_map keys surfaces off the
    # texture name.
    ((-HALF_W, FLOOR_Y - WALL, 260), (HALF_W, FLOOR_Y - 6, 520), "WATER"),

    # machinery blocks — cover, and something for m64_clip_slide to be
    # exercised against
    ((-HALF_W, FLOOR_Y, 120), (-70, 70, 190), "DECK"),
    ((70, FLOOR_Y, 600), (HALF_W, 70, 670), "DECK"),
]

# Statue plinths. Every one of these is a place a GARGOYLE can be standing,
# and with the veil down they are all indistinguishable — that is the point
# (VEIL_DESIGN.md §5). Author more plinths than gargoyles, always.
PLINTHS_Z = [80, 240, 400, 560, 720, 840]
for i, z in enumerate(PLINTHS_Z):
    x = -HALF_W if i % 2 == 0 else HALF_W - 44
    BRUSHES.append(((x, FLOOR_Y, z), (x + 44, 34, z + 44), "STONE"))

# Point entities. classname → actor profile is bound at runtime by
# m64_map_register_classname; anything unrecognised is skipped with a
# debugf rather than failing the load.
ENTITIES = [
    ("info_player_start", (0, 40, -40), 0),
    # Three imps and one is close — the teaching encounter.
    ("info_imp", (-60, 10, 300), 180),
    ("info_imp", (90, 10, 360), 180),
    ("info_imp", (10, 10, 180), 180),
    # The stalker starts far away. It only closes while you are not looking.
    ("info_hellhound", (0, 10, 840), 180),
    # Two of the six plinths are not statues.
    ("info_gargoyle", (-138, 34, 240), 90),
    ("info_gargoyle", (138, 34, 560), 270),
    # The elite holds the far end, and its halo pins the veil up — which
    # freezes the hellhound standing behind it until you kill it.
    ("info_overlord", (0, 10, 780), 180),
]


def box_planes(mins, maxs):
    """Six faces of an axis-aligned box, three points each.

    Point order per face follows the convention in the parent repo's
    assets/*.map: the first triple lies on the min side of the axis, the
    second on the max side, and m64_map takes componentwise min/max over
    all of them to recover the AABB.
    """
    x0, y0, z0 = mins
    x1, y1, z1 = maxs
    return [
        [(x0, y0, z0), (x0, y0, z1), (x0, y1, z0)],  # -X
        [(x1, y1, z1), (x1, y1, z0), (x1, y0, z1)],  # +X
        [(x0, y0, z0), (x1, y0, z0), (x0, y0, z1)],  # -Y
        [(x1, y1, z1), (x0, y1, z1), (x1, y1, z0)],  # +Y
        [(x0, y0, z0), (x0, y1, z0), (x1, y0, z0)],  # -Z
        [(x1, y1, z1), (x1, y0, z1), (x0, y1, z1)],  # +Z
    ]


def emit(out):
    lines = ['{', '"classname" "worldspawn"']
    for mins, maxs, tex in BRUSHES:
        lines.append('{')
        for face in box_planes(mins, maxs):
            pts = " ".join("( %g %g %g )" % p for p in face)
            lines.append("%s %s 0 0 0 1 1" % (pts, tex))
        lines.append('}')
    lines.append('}')

    for classname, origin, yaw in ENTITIES:
        lines.append('{')
        lines.append('"classname" "%s"' % classname)
        lines.append('"origin" "%g %g %g"' % origin)
        lines.append('"angle" "%g"' % yaw)
        lines.append('}')

    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d brushes, %d entities" % (out, len(BRUSHES), len(ENTITIES)))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    emit(ap.parse_args().out)
