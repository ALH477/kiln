#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""pm_world.py — the island hub and the lab, authored FOR this engine.

    blender --background --python pm_world.py -- --model island --out i.gltf

── Why these are generated and not imported ───────────────────────────────
PetaByte-Madness' island and lab began as art drops: `island_n64.obj` is
12,468 triangles of sculpted terrain with no UVs and no normals, and
`dank_lab.obj` is a centimetre-authored mesh whose camera and collision
numbers had to be reverse-engineered from its bounding box. Both look fine
in a viewport and neither cooperates with the runtime:

  * The island had to be decimated to 15% to fit the console, which is what
    turned a rounded island into a flat-topped mesa with vertical cliffs.
  * Nothing in either mesh is FLAT, so `kiln_clip`'s axis-aligned brushes
    have nothing to bind to and every collision volume is a guess.
  * Neither carries named sub-objects, so `kiln_room` has no bounds and
    `kiln_actor` has no spawn anchors — an entrance is wherever someone
    typed a number.

These two models are authored the other way round: the geometry is derived
from the volumes the runtime needs, so a walkable surface is flat BECAUSE a
brush has to match it, and a doorway is a named object BECAUSE something
spawns there. The look is taken from the originals — a low green island
ringed with sand, a cramped underwater station — but the structure serves
the engine.

── The island is a hub, not scenery ───────────────────────────────────────
Ocarina of Time's Hyrule Field: an open middle you always come back to,
with the ways out arranged around it so a direction is a destination. Here
that is a central field, six paths running out at sixty-degree bearings,
and raised ground between the paths so the paths READ as paths rather than
as arbitrary lines on a plain. Six gates, one per path, each a named object
the game can hang a trigger and a room transition on.

Everything walkable is flat and at one of two heights. That is not a
stylistic choice — it is what lets the whole hub be collided with a handful
of AABBs instead of a triangle-soup mesh the console cannot afford to test
against.

── Units ──────────────────────────────────────────────────────────────────
Authored in metres. The exporter multiplies by 64 (`--base-scale=64`), and
this world runs at 64 units to the metre, so one Blender unit here is one
metre in the game and no call site has to guess a scale. See pm_lab.h.

Blender is +Z up; the exporter converts to the engine's +Y up.

── Testable without Blender ───────────────────────────────────────────────
Every builder returns plain (verts, faces, colors) and imports no bpy, so
tools/blender/test_world.py checks the shape of the result — flat surfaces
really flat, gates on their bearings, triangle budgets — with a bare
python3. Same discipline as pm_env.py and quake_map.py.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ── Palette ────────────────────────────────────────────────────────────
# Few, flat, separated in VALUE — the rule pm_props.py's _island_colors
# states and docs/VEIL_DESIGN.md §4 requires, because the veil collapses hue
# and anything distinguished only by hue stops being distinguishable.
# A Florida caye, not a highland. The Keys are coral rubble and sand with
# scrub and mangrove on top: the value range is narrow and warm, and the
# darkest thing on the island is the mangrove at the waterline rather than a
# hillside. Still separated by VALUE first (docs/VEIL_DESIGN.md).
SAND      = (176, 162, 128)  # dry coral sand, the brightest land
WET_SAND  = (118, 108, 88)   # the tide line
SCRUB     = (86, 96, 66)     # sea grape and buttonwood
FIELD     = (104, 112, 78)   # the open middle: thin grass over sand
PATH      = (158, 146, 116)  # a sand track, reads by VALUE against scrub
ROCK      = (138, 130, 112)  # exposed caprock
MANGROVE  = (44, 58, 44)     # the dark fringe where land meets water
GATE_STONE = (138, 132, 120)
GATE_DARK  = (28, 30, 34)  # the opening itself: a hole, not a door

# The temple. Aztec step-pyramid in weathered limestone, with a white
# shrine on top banded in blue — the one saturated colour on the island,
# which is what makes it read as built rather than grown.
TEMPLE_STONE = (128, 120, 104)
TEMPLE_STEP  = (104, 97, 84)   # the tread faces, a step darker
TEMPLE_WHITE = (226, 224, 214)
TEMPLE_BLUE  = (54, 92, 158)

# Lab
DECK      = (92, 96, 100)
BULKHEAD  = (66, 72, 78)
TRIM      = (120, 112, 84)
WATER     = (28, 46, 62)
LAMP      = (198, 160, 92)

# ── Island dimensions, in metres ───────────────────────────────────────
# Sized so the caye is roughly half a large OoT-style hub field, split
# 50/50 land:water by AREA — not a uniform rescale of the old ~200 m
# island, because the old caye was already ~2:1 land:water by area. Hitting
# 50/50 means the WATER band has to grow disproportionately more than the
# land bands: land radii scale ~2.0x (measured land radius ~103 m -> ~210 m),
# the water/shelf radius scales ~2.36x (measured shelf radius ~126 m ->
# ~300 m), so r_water ends up ~sqrt(2) x r_land rather than tracking it.
# The camera (pm_demo.c's ORBIT_RADII etc.) is expressed as a FRACTION of
# PM_LAND_RADIUS and rescales itself; near_z/far_z there are absolute and
# must be rescaled by hand alongside this — see pm_demo.c's comment on that.
# Iterate these nominal (pre-wobble) inputs against `--emit-header`'s
# emitted PM_LAND_RADIUS/PM_ISLAND_RADIUS (the post-wobble MEASURED radii)
# rather than trusting this arithmetic by hand — the wobble inflates each
# band by a different factor, which is exactly the kind of drift this
# generator's self-measurement discipline exists to catch.
R_FIELD   = 84.0   # flat central field
R_INNER   = 94.0   # field rolls off / hills begin
R_GATE    = 124.0  # the gate plazas
R_SHORE   = 158.0  # sand begins (a narrow strand, not a apron)
# The mesh runs well past the waterline as a SHALLOW SHELF. A caye does not
# drop off at its beach — it sits in the middle of a flat that stays
# ankle-to-waist deep for a long way out, and that broad turquoise band is
# most of what makes an aerial read as the Keys rather than as an island in
# deep ocean. pm_env's sea colours the water above it (SHALLOW/REEF).
# Scaled 2.36x rather than 2.0x (unlike the land bands above) so the shelf
# annulus's area comes out equal to the land disk's — the 50/50 split.
R_WATER   = 245.0
# The rim continues under the water rather than stopping at z = 0.
#
# Stopping at sea level puts the island's outer ring and the sea plane at the
# same height in the same place, and two coplanar surfaces at equal depth is
# a shimmering seam that moves with the camera - it reads as the island
# glitching at the waterline. Carrying the skirt down means the sea plane
# cuts through solid ground and the shoreline is simply where the terrain
# crosses zero, which is also where a real one is.
# Barely submerged: the shelf is shallow, which is the whole point. Deep
# enough that the sea plane never touches it (that was the shimmering
# coplanar seam), shallow enough to read as a flat rather than a drop.
UNDERWATER_Z = -3.0

# Key West's highest natural ground is about 5.5 m and most of the Keys sit
# at one or two. These were 7 and 27, which is a headland; at that height the
# island reads as a green mountain with a beach stuck round it.
FIELD_Z   = 2.4    # the open middle, barely above the tide
HILL_Z    = 6.0    # dune and scrub crests between the paths
GATE_Z    = 2.4    # plazas level with the field, so the paths stay flat

GATE_COUNT   = 6
PATH_HALF_DEG = 9.0   # half-width of a path corridor, in degrees
# Must divide GATE_COUNT evenly so every gate lands on a vertex column
# rather than straddling one, and PATH_HALF_DEG must exceed half a sector or
# a path can fall between columns and vanish.
# Low poly on purpose. 54 sectors and density 1.15 gave 2,106 triangles of
# smoothly-curved terrain, which is neither cheap nor the look — a caye is
# flat sand and scrub and wants to read as chunky facets. 30 divides the six
# gates evenly (5 columns each) and stays wider than PATH_HALF_DEG.
#
# SECTORS stays fixed across the caye resize on purpose: the facet count
# is an angular-resolution choice about the LOOK (chunky, not smooth), and
# bigger facets at a bigger scale still read as chunky, not as low-res.
# TERRAIN_DENSITY (the RADIAL ring count) went up modestly, not linearly
# with the ~4x area increase, so ring spacing doesn't get visibly coarser
# now that every band is wider — see test_world.py's raised triangle budget
# for the resulting count.
SECTORS      = 30     # 12 degrees each; 30 / 6 gates = 5 columns per gate
TERRAIN_DENSITY = 0.9  # multiplier on the per-band ring counts below


def gate_bearings():
    """Compass bearings of the six gates, in degrees.

    Offset by half a sector so every gate sits on a vertex column rather
    than straddling one — a path centred on a quad edge comes out one
    column wider on one side and reads as crooked from the air.
    """
    step = 360.0 / GATE_COUNT
    return [i * step for i in range(GATE_COUNT)]


def _ang_delta(a, b):
    """Smallest absolute difference between two bearings, in degrees."""
    d = abs((a - b) % 360.0)
    return min(d, 360.0 - d)


def path_strength(theta_deg):
    """1.0 inside a path corridor, 0.0 outside, with a short blend.

    The blend is what keeps the hills from meeting the paths as a cliff:
    the runtime collides the paths as flat brushes, so the walkable width
    is the FULL-strength part and the ramp is scenery either side of it.
    """
    nearest = min(_ang_delta(theta_deg, b) for b in gate_bearings())
    if nearest <= PATH_HALF_DEG:
        return 1.0
    blend = PATH_HALF_DEG + 7.0
    if nearest >= blend:
        return 0.0
    return 1.0 - (nearest - PATH_HALF_DEG) / (blend - PATH_HALF_DEG)


def coast_wobble(theta_deg):
    """A multiplier on the outer radii, so the coastline is not a circle.

    Three low harmonics, chosen and not random: this pipeline forbids
    `random` (kilnlib.py's header states the rule) so a rebuild is
    byte-identical. Three is enough that no lobe repeats within a turn and
    few enough that the shape stays legible from the air.
    """
    a = math.radians(theta_deg)
    return (1.0
            + 0.115 * math.sin(3.0 * a + 0.7)
            + 0.070 * math.sin(5.0 * a + 2.1)
            + 0.040 * math.sin(7.0 * a + 4.3))


def ridge_scale(theta_deg):
    """Per-bearing multiplier on ridge height, so the six are not identical.

    Six matching mounds around a disc reads as a machine part. Varying them
    is most of what makes the silhouette read as land."""
    a = math.radians(theta_deg)
    return 0.72 + 0.38 * (0.5 + 0.5 * math.sin(2.0 * a + 1.15)) \
                + 0.16 * (0.5 + 0.5 * math.sin(3.0 * a + 3.9))


def _smoothstep(t):
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


def island_height(r, theta_deg):
    """Height in metres at a polar coordinate, and the surface class.

    Returns (z, kind) where kind is one of field/path/hill/sand/wet/rock.
    One function owns both so the colour can never disagree with the shape —
    a bright path painted up the side of a hill is the kind of mistake that
    only shows up on a console.
    """
    if r <= R_FIELD:
        return FIELD_Z, "field"

    if r <= R_GATE:
        # Between the field and the gates: paths stay level, the ground
        # between them rises into ridges.
        p = path_strength(theta_deg)
        t = _smoothstep((r - R_FIELD) / (R_GATE - R_FIELD))
        # Ridges peak midway out and settle again at the gate ring, so the
        # gates are not buried in a hillside.
        ridge = math.sin(t * math.pi) * (HILL_Z - FIELD_Z) \
                * ridge_scale(theta_deg)
        z = FIELD_Z + ridge * (1.0 - p)
        if p >= 1.0:
            return GATE_Z, "path"
        return z, "hill" if p < 0.5 else "path"

    if r <= R_SHORE:
        # Outside the gates the whole ring falls away to the beach.
        t = _smoothstep((r - R_GATE) / (R_SHORE - R_GATE))
        p = path_strength(theta_deg)
        top = GATE_Z if p > 0.5 else FIELD_Z + (HILL_Z - FIELD_Z) * 0.25
        z = top * (1.0 - t) + 1.2 * t
        return z, "rock" if p <= 0.5 and t < 0.5 else "sand"

    # The beach, then the shelf. The drop happens in the FIRST part of this
    # band and then flattens out, so the profile is a strand that gives way
    # to a long shallow flat rather than a ramp to deep water.
    t = _smoothstep((r - R_SHORE) / (R_WATER - R_SHORE))
    drop = _smoothstep(min(1.0, t * 2.6))
    z = 1.0 * (1.0 - drop) + UNDERWATER_Z * drop
    if t < 0.22:
        return z, "sand"
    if t < 0.40:
        return z, "mangrove"   # the dark fringe right at the tide line
    return z, "wet"


KIND_COLOR = {
    "field": FIELD, "path": PATH, "hill": SCRUB,
    "rock": ROCK, "sand": SAND, "wet": WET_SAND,
    "mangrove": MANGROVE,
}

# The terrain texture's stripe order — a CONTRACT with tools/gen_textures.py's
# _BAND_SEEDS/_BAND_BASE/_BAND_AMP (index i here must be index i there).
# Chosen so kinds that actually sit next to each other (radially as a ring's
# `r` grows, or angularly between a path corridor and the hillside beside
# it) land on adjacent stripes: field borders path/hill, hill fades toward
# rock, rock gives way to sand, sand to mangrove to wet. This can't be
# perfect for every real adjacency (a path vertex can radially border a sand
# vertex, skipping hill/rock) — this hardware has no per-triangle
# multi-texture blend to fall back on, so UV interpolation across one shared
# atlas plus the RDP's bilinear filter is the whole blending budget; see
# tools/gen_textures.py's tex_terrain_bands docstring.
TERRAIN_BAND_KINDS = ("field", "path", "hill", "rock", "sand", "mangrove", "wet")


def terrain_rings(density=TERRAIN_DENSITY):
    """Ring radii, DERIVED from the band radii rather than listed.

    This used to be a literal list, and when the island was rescaled the
    literals stayed put: three consecutive pairs ended up running BACKWARDS
    (70 -> 62, 87 -> 79, 96 -> 83) so the quad bands folded back through
    each other, and two rings sat beyond R_WATER where the height function
    clamps, adding a flat skirt of dead geometry. Deriving them makes the
    list monotonic by construction and makes it impossible to move a band
    radius without the rings following.

    Density is per band, not uniform: the field is flat and needs almost no
    rings, while the ridges and the waterline carry the whole silhouette.
    """
    bands = (
        (0.0,     R_FIELD, 3),   # flat middle
        (R_FIELD, R_GATE,  6),   # the ridges and the paths between them
        (R_GATE,  R_SHORE, 4),   # the fall from the plazas to the beach
        (R_SHORE, R_WATER, 5),   # the strand, then the shallow shelf
    )
    out = [0.0]
    for lo, hi, n in bands:
        n = max(1, int(round(n * density)))
        for i in range(1, n + 1):
            out.append(lo + (hi - lo) * (float(i) / n))
    return out


def _band_uv(kind, s):
    """UV for a terrain vertex: U selects the kind's stripe in
    tools/gen_textures.py's terrain_bands.i8.png atlas (see
    TERRAIN_BAND_KINDS), V walks once around the island per revolution so
    the atlas gets spatial variation rather than one flat sampled texel.
    """
    u = (TERRAIN_BAND_KINDS.index(kind) + 0.5) / float(len(TERRAIN_BAND_KINDS) + 1)
    v = float(s) / SECTORS
    return (u, v)


def build_island_terrain(rings=None):
    """The polar heightfield. Returns (verts, faces, colors, uvs)."""
    if rings is None:
        rings = terrain_rings()

    verts, colors, uvs = [], [], []
    # The centre is one vertex, so the first ring fans rather than quads.
    verts.append((0.0, 0.0, FIELD_Z))
    colors.append(FIELD)
    uvs.append(_band_uv("field", 0))

    for r in rings[1:]:
        for s in range(SECTORS):
            theta = 360.0 * s / SECTORS
            a = math.radians(theta)
            # Height is classified from the NOMINAL ring radius so the
            # field/path/hill bands stay concentric and the collision
            # brushes written against them stay true. Only the drawn
            # position is displaced, and only outside the gate ring, so the
            # coastline wanders while the playable middle does not.
            z, kind = island_height(r, theta)
            # The wobble reaches in as far as the field's edge. Confining
            # it to beyond the gates made an island that was a perfect
            # disc with a wavy hem; letting it run inward makes the whole
            # outer half an irregular shape, while the flat middle — the
            # part the collision brushes describe — stays exactly circular.
            blend = 0.0
            if r > R_FIELD:
                blend = _smoothstep((r - R_FIELD) / (R_WATER - R_FIELD))
            rr = r * (1.0 + (coast_wobble(theta) - 1.0) * blend)
            verts.append((math.cos(a) * rr, math.sin(a) * rr, z))
            colors.append(KIND_COLOR[kind])
            uvs.append(_band_uv(kind, s))

    faces = []
    # Fan from the centre to the first full ring.
    for s in range(SECTORS):
        nxt = (s + 1) % SECTORS
        faces.append((0, 1 + s, 1 + nxt))
    # Quad rings after that.
    for ring in range(len(rings) - 2):
        base = 1 + ring * SECTORS
        nxt_base = base + SECTORS
        for s in range(SECTORS):
            nxt = (s + 1) % SECTORS
            faces.append((base + s, nxt_base + s, nxt_base + nxt, base + nxt))

    return verts, faces, colors, uvs


def _box(cx, cy, cz, sx, sy, sz):
    """Axis-aligned box as (verts, faces), centred on (cx,cy) sitting at cz.

    Local rather than kilnlib's: kilnlib imports bpy at module scope, and
    everything above has to stay importable for the tests.
    """
    hx, hy = sx * 0.5, sy * 0.5
    v = [(cx - hx, cy - hy, cz), (cx + hx, cy - hy, cz),
         (cx + hx, cy + hy, cz), (cx - hx, cy + hy, cz),
         (cx - hx, cy - hy, cz + sz), (cx + hx, cy - hy, cz + sz),
         (cx + hx, cy + hy, cz + sz), (cx - hx, cy + hy, cz + sz)]
    f = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
         (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    return v, f


def _cyl(cx, cy, cz, r_bot, r_top, h, segs=8):
    """Cylinder / cone frustum. Local, for the same reason _box is."""
    verts, faces = [], []
    for i in range(segs):
        a = 2.0 * math.pi * i / segs
        verts.append((cx + math.cos(a) * r_bot, cy + math.sin(a) * r_bot, cz))
    for i in range(segs):
        a = 2.0 * math.pi * i / segs
        verts.append((cx + math.cos(a) * r_top, cy + math.sin(a) * r_top, cz + h))
    for i in range(segs):
        j = (i + 1) % segs
        faces.append((i, j, segs + j, segs + i))
    faces.append(tuple(range(segs - 1, -1, -1)))
    faces.append(tuple(range(segs, segs * 2)))
    return verts, faces


def _rot_y(verts, deg, ox=0.0, oz=0.0):
    """Tilt about the tangent axis — for leaning stones and wrecks."""
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    out = []
    for (x, y, z) in verts:
        dx, dz = x - ox, z - oz
        out.append((ox + dx * c - dz * s, y, oz + dx * s + dz * c))
    return out


class _Part:
    """Accumulates geometry in a gate's LOCAL space.

    x runs along the tangent (across the mouth), y runs radially OUTWARD
    from the island centre (into the mound), z is up from the plaza. Each
    entrance builder works in these axes and build_gates rotates the result
    onto its bearing, so an entrance can be authored as if it faced you.
    """

    def __init__(self):
        self.v, self.f, self.c = [], [], []

    def add(self, verts, faces, col):
        base = len(self.v)
        self.v.extend(verts)
        self.f.extend(tuple(i + base for i in face) for face in faces)
        self.c.extend([col] * len(verts))

    def box(self, cx, cy, cz, sx, sy, sz, col):
        v, f = _box(cx, cy, cz, sx, sy, sz)
        self.add(v, f, col)

    def cyl(self, cx, cy, cz, rb, rt, h, col, segs=8):
        v, f = _cyl(cx, cy, cz, rb, rt, h, segs)
        self.add(v, f, col)

    def lean(self, cx, cy, cz, sx, sy, sz, deg, col):
        v, f = _box(cx, cy, cz, sx, sy, sz)
        self.add(_rot_y(v, deg, cx, cz), f, col)

    def shaft(self, rings, w, h, back, col_from=None):
        """The dark recess every entrance needs: receding, darkening rings.

        This is the part that makes a hole read as a way IN rather than as a
        black rectangle painted on a wall, so every entrance gets one no
        matter how different its architecture is."""
        src = col_from or (78, 78, 84)
        for i in range(rings):
            t = (i + 1) / float(rings)
            # Reaches GATE_DARK exactly at the back, whatever the facade is
            # made of. Fading only PART of the way toward it left a pale
            # entrance with a pale hole: the ruin's deepest ring came out at
            # 127 where the basalt maw's was 101, so the same architecture
            # read as "a way in" on one gate and "a recess" on another. The
            # exponent puts most of the darkening in the first ring, which
            # is where the eye decides whether it is looking at a hole.
            shade = (1.0 - t) ** 1.6
            col = tuple(max(0, int(GATE_DARK[k] + (src[k] - GATE_DARK[k]) * shade))
                        for k in range(3))
            self.box(0.0, back + i * 1.6, 0.0,
                     w - t * 0.5, 1.7, h - t * 0.4, col)


# Tunnel mouth dimensions, in metres.
GATE_OPEN_W   = 4.4     # the opening
GATE_OPEN_H   = 5.0
GATE_JAMB     = 1.5     # stone either side
GATE_LINTEL   = 1.4
GATE_BERM_W   = 15.0    # the mound it is cut into
GATE_BERM_D   = 9.0
GATE_BERM_H   = 7.5
GATE_SHAFT    = 4       # receding rings that give the hole depth


# Extra materials the entrances want. Ominous means low value and low
# saturation with ONE thing that is neither — rust, or bone.
MOSS      = (52, 64, 46)
CONCRETE  = (108, 108, 104)
RUST      = (112, 66, 40)
BONE      = (198, 190, 168)
BASALT    = (58, 58, 64)


def _ent_cenote(p):
    """0 — a sinkhole. The way in is DOWN, which is the most unsettling
    entrance a flat island can offer: no facade, no promise, just a hole
    with water somewhere below."""
    for i in range(3):                       # stepped rim, descending
        t = i / 2.0
        p.cyl(0.0, 2.0, -0.4 - i * 1.1, 7.0 - i * 1.4, 6.2 - i * 1.4,
              1.1, _lerp_c(ROCK, BASALT, t), segs=10)
    p.cyl(0.0, 2.0, -4.2, 4.0, 3.4, 3.0, (16, 20, 26), segs=10)   # the dark
    p.box(0.0, 2.0, -5.0, 6.0, 6.0, 0.4, (24, 40, 48))            # water below
    for a in (-1, 1):                        # two leaning slabs, like teeth
        p.lean(a * 5.4, -1.4, 0.0, 1.2, 1.6, 4.2, a * 13.0, ROCK)


def _ent_ruin(p):
    """1 — a collapsed arch. One jamb still standing, the lintel dropped and
    tilted across the gap, rubble where the other side used to be."""
    p.box(-3.2, 0.0, 0.0, 1.6, 2.0, 6.4, GATE_STONE)
    p.lean(3.2, 0.0, 0.0, 1.6, 2.0, 4.6, 9.0, GATE_STONE)
    p.lean(0.4, 0.0, 5.0, 8.4, 1.8, 1.2, -12.0, GATE_STONE)       # fallen lintel
    p.box(2.6, -2.2, 0.0, 2.2, 1.8, 1.0, ROCK)                    # rubble
    p.box(-2.0, -3.0, 0.0, 1.4, 1.2, 0.7, ROCK)
    p.box(0.0, 3.0, 0.0, 9.0, 5.0, 5.0, MOSS)                     # the mound
    p.shaft(4, 4.2, 5.0, 1.2, GATE_STONE)


def _ent_maw(p):
    """2 — a carved mouth. Teeth top and bottom around a wide low opening.
    The one entrance that is unambiguously a threat rather than a ruin."""
    p.box(0.0, 3.2, 0.0, 13.0, 6.0, 7.0, BASALT)
    for i in range(5):                        # upper teeth
        x = -4.0 + i * 2.0
        p.lean(x, 0.0, 3.4, 1.1, 1.6, 1.8, 180.0, BONE)
    for i in range(4):                        # lower
        x = -3.0 + i * 2.0
        p.box(x, 0.0, 0.0, 1.0, 1.6, 1.3, BONE)
    p.shaft(4, 8.0, 3.4, 1.4, BASALT)
    for a in (-1, 1):                         # brow
        p.lean(a * 5.0, 0.2, 4.6, 3.6, 1.4, 1.2, a * -14.0, BASALT)


def _ent_bunker(p):
    """3 — a concrete blockhouse. The Keys are full of them, and a square
    of poured concrete on a coral island is its own kind of wrong."""
    p.box(0.0, 3.0, 0.0, 12.0, 7.0, 5.2, CONCRETE)
    p.box(0.0, -0.6, 0.0, 6.4, 1.6, 4.2, CONCRETE)      # entry throat
    p.box(0.0, -0.7, 3.6, 7.4, 2.0, 0.9, CONCRETE)      # brow
    p.box(-4.6, 0.4, 1.8, 2.6, 0.6, 0.5, (28, 30, 32))  # gun slit
    p.box(4.6, 0.4, 1.8, 2.6, 0.6, 0.5, (28, 30, 32))
    p.box(0.0, -1.6, 0.0, 7.0, 0.5, 0.35, RUST)         # rusted sill
    p.box(6.6, 1.4, 0.0, 3.0, 4.0, 1.6, SAND)           # drifted sand
    p.shaft(4, 3.8, 4.2, 1.0, CONCRETE)


def _ent_wreck(p):
    """4 — a hull driven onto the shelf and left. You go in through the
    broken side; the ribs are still standing."""
    p.lean(0.0, 4.0, 0.4, 6.0, 14.0, 5.0, -14.0, RUST)   # the hull, canted
    for i in range(4):                                    # ribs
        y = -0.6 + i * 2.2
        p.lean(0.0, y, 0.0, 7.4, 0.5, 4.2, -14.0, (74, 48, 32))
    p.box(0.0, -1.4, 0.0, 5.0, 1.6, 4.0, (40, 28, 22))   # the breach
    p.box(-4.4, -2.0, 0.0, 2.0, 2.4, 0.9, SAND)
    p.shaft(4, 4.4, 4.0, 0.6, (74, 48, 32))


def _ent_monolith(p):
    """5 — standing stones over a stair going down. No structure at all:
    just two things put there by someone, and a way under."""
    for a in (-1, 1):
        p.lean(a * 4.2, 0.0, 0.0, 1.8, 1.8, 9.0, a * 6.0, BASALT)
        p.box(a * 4.2, 0.0, 9.0, 2.4, 2.2, 0.8, BASALT)
    for i in range(4):                                    # descending steps
        p.box(0.0, -1.2 + i * 1.3, -i * 0.9, 5.0, 1.3, 0.9,
              _lerp_c(ROCK, BASALT, i / 3.0))
    p.box(0.0, 3.4, -3.2, 5.6, 4.0, 3.4, (18, 20, 26))    # the dark under
    p.box(0.0, 3.4, 0.4, 7.0, 4.6, 0.7, BASALT)           # capstone over it
    p.shaft(3, 4.6, 3.0, 4.6, BASALT)


def _lerp_c(c0, c1, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(a + (b - a) * t)) for a, b in zip(c0, c1))


ENTRANCES = (_ent_cenote, _ent_ruin, _ent_maw,
             _ent_bunker, _ent_wreck, _ent_monolith)


def build_gates():
    """Six entrances, one per bearing — and six DIFFERENT entrances.

    ── Why they are not all the same ───────────────────────────────────
    They were: one berm-and-facade repeated six times. On a hub whose whole
    navigational premise is that a direction is a destination, six identical
    doors is the one thing that breaks it — you cannot tell where you have
    already been. Now each is its own object with its own architecture: a
    sinkhole, a collapsed arch, a carved maw, a concrete bunker, a wrecked
    hull, and standing stones over a stair.

    What they share is the part that makes an entrance an entrance: a dark
    recess with DEPTH (see _Part.shaft). An opening with nothing behind it
    reads as decoration however it is dressed.

    Each builder works in local axes — x across the mouth, y radially out,
    z up — and is rotated onto its bearing here, so entrances can be
    authored facing the reader.
    """
    verts, faces, colors = [], [], []

    for gi, bearing in enumerate(gate_bearings()):
        part = _Part()
        ENTRANCES[gi % len(ENTRANCES)](part)

        a = math.radians(bearing)
        cx, cy = math.cos(a) * R_GATE, math.sin(a) * R_GATE
        tx, ty = -math.sin(a), math.cos(a)      # local +x
        rx, ry = math.cos(a), math.sin(a)       # local +y, radially out

        base = len(verts)
        for (lx, ly, lz) in part.v:
            verts.append((cx + tx * lx + rx * ly,
                          cy + ty * lx + ry * ly,
                          GATE_Z + lz))
        faces.extend(tuple(i + base for i in face) for face in part.f)
        colors.extend(part.c)

    return verts, faces, colors


# Temple dimensions, in metres.
TEMPLE_TIERS   = 5
TEMPLE_BASE    = 30.0   # the bottom tier, across
TEMPLE_TOP     = 12.0   # the top tier, across
TEMPLE_TIER_H  = 3.2
SHRINE_W       = 9.0
SHRINE_BANDS   = 5      # white/blue/white/blue/white
SHRINE_BAND_H  = 1.3


def build_landmark():
    """The temple at the island's centre — where every path leads.

    An Aztec step-pyramid: square tiers narrowing as they rise, a stair up
    one face, and a white shrine on top banded horizontally in blue.

    ── Why this and not a tower ────────────────────────────────────────
    Hyrule Field is navigable because the castle is visible from all of it.
    This does that job, and it also gives the six gates something to be
    gates TO: the paths do not merely radiate, they converge on a building.

    ── Why the stripes are geometry ────────────────────────────────────
    The shrine is a STACK of thin slabs rather than one box with banded
    vertex colours. A box only has vertices at its corners, so colouring
    bands into it would need it subdivided anyway — and subdividing into
    slabs gives crisp edges where interpolated vertex colours would give a
    gradient. Same triangle count, better result.
    """
    verts, faces, colors = [], [], []

    def add(v, f, col):
        base = len(verts)
        verts.extend(v)
        faces.extend(tuple(i + base for i in face) for face in f)
        colors.extend([col] * len(v))

    # ── The stepped body ────────────────────────────────────────────────
    z = FIELD_Z
    for i in range(TEMPLE_TIERS):
        t = i / float(TEMPLE_TIERS - 1)
        w = TEMPLE_BASE + (TEMPLE_TOP - TEMPLE_BASE) * t
        v, f = _box(0.0, 0.0, z, w, w, TEMPLE_TIER_H)
        # Alternate the tier colour slightly so the steps read from the air,
        # where the silhouette alone would be a smooth cone.
        add(v, f, TEMPLE_STONE if (i % 2 == 0) else TEMPLE_STEP)
        z += TEMPLE_TIER_H

    # ── The stair ───────────────────────────────────────────────────────
    # Up the face that looks toward gate 0, so the approach from the
    # island's main path arrives at the bottom of the steps.
    a = math.radians(gate_bearings()[0])
    nx, ny = math.cos(a), math.sin(a)
    stair_w = 7.0
    sz = FIELD_Z
    for i in range(TEMPLE_TIERS):
        t = i / float(TEMPLE_TIERS - 1)
        w = TEMPLE_BASE + (TEMPLE_TOP - TEMPLE_BASE) * t
        # Each flight sits against its tier and juts out half a metre.
        out = w * 0.5 + 0.6
        v, f = _box(nx * out, ny * out, sz, stair_w, 2.2, TEMPLE_TIER_H)
        add(v, f, TEMPLE_STEP)
        sz += TEMPLE_TIER_H

    # ── The shrine ──────────────────────────────────────────────────────
    for i in range(SHRINE_BANDS):
        col = TEMPLE_WHITE if (i % 2 == 0) else TEMPLE_BLUE
        v, f = _box(0.0, 0.0, z, SHRINE_W, SHRINE_W, SHRINE_BAND_H)
        add(v, f, col)
        z += SHRINE_BAND_H

    # A flat white lintel to cap it, so the top band is not a stripe.
    v, f = _box(0.0, 0.0, z, SHRINE_W + 1.0, SHRINE_W + 1.0, 0.8)
    add(v, f, TEMPLE_WHITE)

    return verts, faces, colors


# ── The lab ────────────────────────────────────────────────────────────
# Three zones, which is what dank_lab_gen.py's own header says the original
# had before the OBJ export flattened them into materials: a work room, a
# hall, and the dock that holds the moon pool and the LOACH.
#
# Built from boxes with FLAT floors at a single height, so pm_lab.c's
# collision brushes are the same numbers as the geometry rather than an
# approximation of it.
LAB_H      = 2.6    # ceiling height, metres
WALL_T     = 0.3
ROOM_W, ROOM_D = 10.0, 6.0
HALL_W, HALL_D = 3.0, 7.0
DOCK_W, DOCK_D = 9.0, 7.0
POOL_W, POOL_D = 5.0, 3.6


def _room_shell(cx, cy, w, d, name_colors, floor_col, wall_col, ceil=True):
    """Floor, four walls and a ceiling for one rectangular zone."""
    verts, faces, colors = [], [], []

    def add(v, f, col):
        base = len(verts)
        verts.extend(v)
        faces.extend(tuple(i + base for i in face) for face in f)
        colors.extend([col] * len(v))

    v, f = _box(cx, cy, -WALL_T, w, d, WALL_T)
    add(v, f, floor_col)
    if ceil:
        v, f = _box(cx, cy, LAB_H, w, d, WALL_T)
        add(v, f, wall_col)
    return verts, faces, colors


def build_lab():
    """The station: room + hall + dock, with the moon pool in the dock."""
    verts, faces, colors = [], [], []

    def add(v, f, col):
        base = len(verts)
        verts.extend(v)
        faces.extend(tuple(i + base for i in face) for face in f)
        colors.extend([col] * len(v))

    def shell(cx, cy, w, d, floor_col):
        v, f, c = _room_shell(cx, cy, w, d, None, floor_col, BULKHEAD)
        base = len(verts)
        verts.extend(v)
        faces.extend(tuple(i + base for i in face) for face in f)
        colors.extend(c)

    # Zones laid along -Y so the dock is at the far end from the entrance,
    # which is the order the intro walks them in.
    room_y = 0.0
    hall_y = -(ROOM_D * 0.5 + HALL_D * 0.5)
    dock_y = hall_y - (HALL_D * 0.5 + DOCK_D * 0.5)

    shell(0.0, room_y, ROOM_W, ROOM_D, DECK)
    shell(0.0, hall_y, HALL_W, HALL_D, DECK)
    shell(0.0, dock_y, DOCK_W, DOCK_D, DECK)

    # Perimeter walls, drawn as boxes so the interior is a closed volume.
    # Gaps are left where the zones meet, which is what makes them one space.
    def wall(cx, cy, w, d, col=BULKHEAD):
        v, f = _box(cx, cy, 0.0, w, d, LAB_H)
        add(v, f, col)

    # Work room
    wall(-ROOM_W * 0.5, room_y, WALL_T, ROOM_D)
    wall(ROOM_W * 0.5, room_y, WALL_T, ROOM_D)
    wall(0.0, room_y + ROOM_D * 0.5, ROOM_W, WALL_T)
    side = (ROOM_W - HALL_W) * 0.5
    wall(-(HALL_W * 0.5 + side * 0.5), room_y - ROOM_D * 0.5, side, WALL_T)
    wall(+(HALL_W * 0.5 + side * 0.5), room_y - ROOM_D * 0.5, side, WALL_T)

    # Hall
    wall(-HALL_W * 0.5, hall_y, WALL_T, HALL_D)
    wall(HALL_W * 0.5, hall_y, WALL_T, HALL_D)

    # Dock
    wall(-DOCK_W * 0.5, dock_y, WALL_T, DOCK_D)
    wall(DOCK_W * 0.5, dock_y, WALL_T, DOCK_D)
    wall(0.0, dock_y - DOCK_D * 0.5, DOCK_W, WALL_T)
    dside = (DOCK_W - HALL_W) * 0.5
    wall(-(HALL_W * 0.5 + dside * 0.5), dock_y + DOCK_D * 0.5, dside, WALL_T)
    wall(+(HALL_W * 0.5 + dside * 0.5), dock_y + DOCK_D * 0.5, dside, WALL_T)

    # The moon pool: a recess in the dock floor with water in it. The water
    # sits slightly below the deck so the lip reads as an edge you could
    # fall over, which is why pm_lab.c blocks it rather than letting the
    # player swim.
    v, f = _box(0.0, dock_y, -0.55, POOL_W, POOL_D, 0.45)
    add(v, f, WATER)

    # Benches down the work room's long wall, and the slab. Both are
    # obstacles the collision already knows about (pm_lab.c's LAB_BRUSHES),
    # so they exist here to make those brushes visible rather than to add
    # detail for its own sake.
    v, f = _box(0.0, room_y + ROOM_D * 0.5 - 0.6, 0.0, ROOM_W - 1.4, 0.8, 0.9)
    add(v, f, TRIM)
    v, f = _box(-ROOM_W * 0.5 + 1.6, room_y - 1.0, 0.0, 2.0, 2.4, 0.7)
    add(v, f, TRIM)

    # One lamp, because the intro's push-in needs something to push toward.
    v, f = _box(0.0, dock_y - DOCK_D * 0.5 + 0.4, 1.9, 1.0, 0.3, 0.5)
    add(v, f, LAMP)

    return verts, faces, colors


# ── The generated header ───────────────────────────────────────────────
# Every camera and collision bug in this game so far has had the same shape:
# a number typed into C that describes geometry defined in Python, which then
# drifted. The island was placed as though it were a few hundred units
# across when it was 12,813; a lab shot opened 69 units behind a wall; a
# flyover key sat at 1.4 island radii and flew through the ridges.
#
# So the geometry measures ITSELF and emits the numbers. Nothing downstream
# gets to guess at an extent, and test_world.py fails if the checked-in
# header stops matching what the builders produce.
BASE_SCALE = 64  # must match nix/blender.nix's baseScale


def measure():
    """Every dimension the runtime needs, in WORLD units."""
    tv = build_island_terrain()[0]
    gv = build_gates()[0]
    lv = build_landmark()[0]
    bv = build_lab()[0]

    def radius(v):
        return max(math.hypot(p[0], p[1]) for p in v) * BASE_SCALE

    def hi(v):
        return max(p[2] for p in v) * BASE_SCALE

    def lo(v):
        return min(p[2] for p in v) * BASE_SCALE

    # Where the terrain crosses sea level: the outer edge of the LAND, as
    # opposed to island_radius, which now runs out to the submerged shelf.
    # A camera framing the island wants this one — the shelf is water and
    # frames as sea, so orbiting against the shelf radius silently pushes
    # the eye half again too far out.
    land_r = R_SHORE
    steps = 200
    for i in range(steps + 1):
        rr = R_SHORE + (R_WATER - R_SHORE) * i / steps
        if island_height(rr, gate_bearings()[0] + 30.0)[0] > 0.0:
            land_r = rr
    land_r *= max(coast_wobble(a * 5.0) for a in range(72))

    gates = []
    for b in gate_bearings():
        a = math.radians(b)
        gates.append((math.cos(a) * R_GATE * BASE_SCALE,
                      GATE_Z * BASE_SCALE,
                      math.sin(a) * R_GATE * BASE_SCALE,
                      b))

    # Where vegetation belongs: the band between the gate plazas and the
    # strand, at whatever height the terrain actually is there. The flyover
    # used to place palms at hardcoded coordinates tuned for a different
    # island, which put them on ridges and in the sea.
    palm_r = (R_GATE + R_SHORE) * 0.5
    # Sampled off a path bearing, since that band is where the palms go.
    palm_y = island_height(palm_r, gate_bearings()[0] + 30.0)[0]

    xs = [p[0] * BASE_SCALE for p in bv]
    ys = [p[1] * BASE_SCALE for p in bv]
    zs = [p[2] * BASE_SCALE for p in bv]

    return {
        "island_radius": radius(tv),
        "land_radius": land_r * BASE_SCALE,
        "island_top": hi(tv + lv),
        "island_bottom": lo(tv),
        "field_radius": R_FIELD * BASE_SCALE,
        "field_y": FIELD_Z * BASE_SCALE,
        "gate_radius": R_GATE * BASE_SCALE,
        "shore_radius": R_SHORE * BASE_SCALE,
        "gates": gates,
        "palm_radius": palm_r * BASE_SCALE,
        "palm_y": palm_y * BASE_SCALE,
        "lab_x0": min(xs), "lab_x1": max(xs),
        "lab_y0": min(zs), "lab_y1": max(zs),
        "lab_z0": min(ys), "lab_z1": max(ys),
    }


def emit_header():
    m = measure()
    L = []
    add = L.append
    add("// SPDX-License-Identifier: MPL-2.0")
    add("//")
    add("// pm_world_gen.h — GENERATED by tools/blender/pm_world.py. Do not edit.")
    add("//")
    add("// Regenerate:  python3 tools/blender/pm_world.py --emit-header \\")
    add("//                  PetaByte-Madness/src/pm_world_gen.h")
    add("//")
    add("// Every value is in WORLD units (the generator authors in metres and")
    add("// the exporter multiplies by %d). These exist so no camera key, no" % BASE_SCALE)
    add("// collision brush and no spawn point has to restate a dimension that")
    add("// the geometry already defines — which is how this game ended up with")
    add("// a shot opening behind a wall and a flyover flying through a ridge.")
    add("")
    add("#ifndef PM_WORLD_GEN_H")
    add("#define PM_WORLD_GEN_H")
    add("")
    add("// ── The island ─────────────────────────────────────────────────────")
    add("#define PM_ISLAND_RADIUS   %.1ff  // to the wobbled coastline"
        % m["island_radius"])
    add("#define PM_LAND_RADIUS     %.1ff  // where terrain crosses sea level"
        % m["land_radius"])
    add("#define PM_ISLAND_TOP      %.1ff  // highest point, incl. the tower"
        % m["island_top"])
    add("#define PM_ISLAND_BOTTOM   %.1ff  // the underwater skirt"
        % m["island_bottom"])
    add("#define PM_FIELD_RADIUS    %.1ff  // the flat walkable middle"
        % m["field_radius"])
    add("#define PM_FIELD_Y         %.1ff  // its height above sea level"
        % m["field_y"])
    add("#define PM_GATE_RADIUS     %.1ff" % m["gate_radius"])
    add("#define PM_SHORE_RADIUS    %.1ff" % m["shore_radius"])
    add("")
    add("// Where vegetation sits: the band between the plazas and the strand,")
    add("// at the height the terrain actually is there.")
    add("#define PM_PALM_RADIUS     %.1ff" % m["palm_radius"])
    add("#define PM_PALM_Y          %.1ff" % m["palm_y"])
    add("")
    add("// ── The six dungeon gates ──────────────────────────────────────────")
    add("// x, y, z and the compass bearing they face, so a room transition or")
    add("// a spawn can be placed at one by index rather than by measurement.")
    add("#define PM_GATE_COUNT %d" % len(m["gates"]))
    add("#define PM_GATE_TABLE { \\")
    for (gx, gy, gz, b) in m["gates"]:
        add("    { %9.1ff, %7.1ff, %9.1ff, %6.1ff }, \\" % (gx, gy, gz, b))
    add("}")
    add("")
    add("// ── The island's own procedural lab (unused) ─────────────────────────")
    add("// The interior box of THIS generator's procedural build_lab(), which")
    add("// is not what ships as PM_MODEL_LAB — that's dank_lab.obj, whose real")
    add("// extents live in PetaByte-Madness/src/pm_lab.h (PM_LAB_REAL_*) and are")
    add("// what pm_lab.c's collision and pm_demo's/pm_intake's cameras actually")
    add("// use. These describe a room nothing currently draws.")
    add("//")
    add("// PM_WORLD_LAB_*, NOT PM_LAB_*. These used to be spelled PM_LAB_X0..Z1,")
    add("// which is exactly what dank_lab_gen.py emits into pm_lab_gen.h — with")
    add("// DIFFERENT numbers (-329.6 here against -451.8 there). Five files")
    add("// include both headers, so which room PM_LAB_REAL_X0 described came down")
    add("// to include order, silently, with only a -Wmacro-redefined warning")
    add("// nobody reads in a build that ships -Wno-error. Two generators must not")
    add("// publish the same name; the one whose room nothing draws is the one")
    add("// that gives it up.")
    add("#define PM_WORLD_LAB_X0  %.1ff" % m["lab_x0"])
    add("#define PM_WORLD_LAB_X1  %.1ff" % m["lab_x1"])
    add("#define PM_WORLD_LAB_Y0  %.1ff" % m["lab_y0"])
    add("#define PM_WORLD_LAB_Y1  %.1ff" % m["lab_y1"])
    add("#define PM_WORLD_LAB_Z0  %.1ff" % m["lab_z0"])
    add("#define PM_WORLD_LAB_Z1  %.1ff" % m["lab_z1"])
    add("")
    add("#endif // PM_WORLD_GEN_H")
    return "\n".join(L) + "\n"


# ── Blender entry point ────────────────────────────────────────────────

def main():
    import kilnlib as m

    name = m.arg("--model")
    m.reset_scene()

    def rgba(cols):
        return [m.srgb(*c) for c in cols]

    if name == "island":
        # Three objects, not one: the game finds the gates by name to hang
        # room transitions on them, and the terrain and the landmark have
        # different collision stories.
        tv, tf, tc, tuv = build_island_terrain()
        m.make_material("terrain")
        m.make_mesh("terrain", tv, tf, "terrain", colors=rgba(tc), uvs=tuv,
                    smooth=False)

        gv, gf, gc = build_gates()
        m.make_material("gates")
        m.make_mesh("gates", gv, gf, "gates", colors=rgba(gc), smooth=False)

        lv, lf, lc = build_landmark()
        m.make_material("tower")
        m.make_mesh("tower", lv, lf, "tower", colors=rgba(lc), smooth=False)

    elif name == "lab":
        v, f, c = build_lab()
        m.make_material("lab")
        m.make_mesh("lab", v, f, "lab", colors=rgba(c), smooth=False)

    else:
        raise SystemExit("pm_world.py: no model '%s' (have: island, lab)" % name)

    m.report()
    m.export_gltf(m.arg("--out"))


if __name__ == "__main__":
    if "--emit-header" in sys.argv:
        out = sys.argv[sys.argv.index("--emit-header") + 1]
        with open(out, "w") as fh:
            fh.write(emit_header())
        sys.stderr.write("pm_world: wrote %s\n" % out)
    elif any(a.startswith("--model") for a in sys.argv):
        main()
