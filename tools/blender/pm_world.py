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
  * Nothing in either mesh is FLAT, so `m64_clip`'s axis-aligned brushes
    have nothing to bind to and every collision volume is a guess.
  * Neither carries named sub-objects, so `m64_room` has no bounds and
    `m64_actor` has no spawn anchors — an entrance is wherever someone
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
SAND      = (132, 116, 84)
WET_SAND  = (86, 78, 60)
GRASS     = (58, 78, 46)
FIELD     = (72, 92, 54)   # the walkable middle, a touch lighter than scrub
PATH      = (122, 116, 98) # trodden stone, clearly lighter than the grass
ROCK      = (96, 92, 86)
HILL      = (44, 56, 38)   # the raised ground between paths, darkest
GATE_STONE = (138, 132, 120)
GATE_DARK  = (28, 30, 34)  # the opening itself: a hole, not a door

# Lab
DECK      = (92, 96, 100)
BULKHEAD  = (66, 72, 78)
TRIM      = (120, 112, 84)
WATER     = (28, 46, 62)
LAMP      = (198, 160, 92)

# ── Island dimensions, in metres ───────────────────────────────────────
# Chosen so the existing flyover camera keys still frame it: the old island
# was 200 m across and the keys orbit 16,000-19,000 world units (250-300 m)
# out. Keeping the footprint means the camera work survives the swap.
# These are scaled so that the WOBBLED coastline — coast_wobble peaks at
# about +20% — lands near 100 m, i.e. a 200 m island, which is what the
# flyover's camera keys were cut for. Sizing the nominal radius to 100 and
# then adding the wobble on top is what made the first version 25% wider
# than the island it replaced, and a camera key that used to sit 2.4 island
# radii out ended up at 1.4 and flew through the hills.
R_FIELD   = 42.0   # flat central field
R_INNER   = 47.0   # field rolls off / hills begin
R_GATE    = 62.0   # the gate plazas
R_SHORE   = 79.0   # sand begins (a narrow strand, not a apron)
R_WATER   = 83.0   # the mesh ends BELOW sea level - see UNDERWATER_Z
# The rim continues under the water rather than stopping at z = 0.
#
# Stopping at sea level puts the island's outer ring and the sea plane at the
# same height in the same place, and two coplanar surfaces at equal depth is
# a shimmering seam that moves with the camera - it reads as the island
# glitching at the waterline. Carrying the skirt down means the sea plane
# cuts through solid ground and the shoreline is simply where the terrain
# crosses zero, which is also where a real one is.
UNDERWATER_Z = -6.0

FIELD_Z   = 7.0    # the field's height above sea level
HILL_Z    = 27.0   # ridge tops between the paths
GATE_Z    = 7.0    # plazas are level with the field, so paths are flat

GATE_COUNT   = 6
PATH_HALF_DEG = 9.0   # half-width of a path corridor, in degrees
# Must divide GATE_COUNT evenly so every gate lands on a vertex column
# rather than straddling one, and PATH_HALF_DEG must exceed half a sector or
# a path can fall between columns and vanish.
SECTORS      = 54     # 6.67 degrees each; 54 / 6 gates = 9 columns per gate
TERRAIN_DENSITY = 1.15 # multiplier on the per-band ring counts below


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
    `random` (m64lib.py's header states the rule) so a rebuild is
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

    # The beach, across the waterline and under it.
    t = _smoothstep((r - R_SHORE) / (R_WATER - R_SHORE))
    z = 1.2 * (1.0 - t) + UNDERWATER_Z * t
    return z, "wet" if t > 0.45 else "sand"


KIND_COLOR = {
    "field": FIELD, "path": PATH, "hill": HILL,
    "rock": ROCK, "sand": SAND, "wet": WET_SAND,
}


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
        (R_SHORE, R_WATER, 4),   # the strand and the underwater skirt
    )
    out = [0.0]
    for lo, hi, n in bands:
        n = max(1, int(round(n * density)))
        for i in range(1, n + 1):
            out.append(lo + (hi - lo) * (float(i) / n))
    return out


def build_island_terrain(rings=None):
    """The polar heightfield. Returns (verts, faces, colors)."""
    if rings is None:
        rings = terrain_rings()

    verts, colors = [], []
    # The centre is one vertex, so the first ring fans rather than quads.
    verts.append((0.0, 0.0, FIELD_Z))
    colors.append(FIELD)

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

    return verts, faces, colors


def _box(cx, cy, cz, sx, sy, sz):
    """Axis-aligned box as (verts, faces), centred on (cx,cy) sitting at cz.

    Local rather than m64lib's: m64lib imports bpy at module scope, and
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


def build_gates():
    """Six gate structures on the gate ring. Returns (verts, faces, colors).

    Each is a trilithon — two posts and a lintel — around a dark recess. It
    is deliberately a silhouette rather than a modelled door: at flyover
    altitude what has to read is "there is a way in there", and six doors
    modelled in detail is six times the triangles for something the player
    only ever sees up close one at a time.
    """
    verts, faces, colors = [], [], []
    for bearing in gate_bearings():
        a = math.radians(bearing)
        cx, cy = math.cos(a) * R_GATE, math.sin(a) * R_GATE
        # Tangent direction, so the gate faces the centre.
        tx, ty = -math.sin(a), math.cos(a)

        def place(ox, oy, oz, sx, sy, sz, col):
            # Offset along the tangent (ox) and the radial (oy).
            px = cx + tx * ox - math.cos(a) * oy
            py = cy + ty * ox - math.sin(a) * oy
            v, f = _box(px, py, GATE_Z + oz, sx, sy, sz)
            base = len(verts)
            verts.extend(v)
            faces.extend(tuple(i + base for i in face) for face in f)
            colors.extend([col] * len(v))

        place(-3.0, 0.0, 0.0, 1.6, 2.2, 7.0, GATE_STONE)   # left post
        place(3.0, 0.0, 0.0, 1.6, 2.2, 7.0, GATE_STONE)    # right post
        place(0.0, 0.0, 7.0, 7.6, 2.2, 1.6, GATE_STONE)    # lintel
        place(0.0, 0.6, 0.0, 4.4, 1.0, 7.0, GATE_DARK)     # the opening
    return verts, faces, colors


def build_landmark():
    """A tower at the island's centre.

    Hyrule Field is navigable because the castle is visible from all of it.
    This is the same job in one object: something tall at the origin that
    tells the player which way they are facing from anywhere on the hub.
    """
    verts, faces, colors = [], [], []
    tiers = [(0.0, 14.0, 12.0, ROCK), (12.0, 10.0, 10.0, GATE_STONE),
             (22.0, 6.0, 8.0, GATE_STONE)]
    for oz, w, h, col in tiers:
        v, f = _box(0.0, 0.0, FIELD_Z + oz, w, w, h)
        base = len(verts)
        verts.extend(v)
        faces.extend(tuple(i + base for i in face) for face in f)
        colors.extend([col] * len(v))
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
    add("// ── The lab ────────────────────────────────────────────────────────")
    add("// The interior box, for pm_lab.c's collision brushes and pm_demo's")
    add("// interior shot. A camera key outside these is outside the room.")
    add("#define PM_LAB_X0  %.1ff" % m["lab_x0"])
    add("#define PM_LAB_X1  %.1ff" % m["lab_x1"])
    add("#define PM_LAB_Y0  %.1ff" % m["lab_y0"])
    add("#define PM_LAB_Y1  %.1ff" % m["lab_y1"])
    add("#define PM_LAB_Z0  %.1ff" % m["lab_z0"])
    add("#define PM_LAB_Z1  %.1ff" % m["lab_z1"])
    add("")
    add("#endif // PM_WORLD_GEN_H")
    return "\n".join(L) + "\n"


# ── Blender entry point ────────────────────────────────────────────────

def main():
    import m64lib as m

    name = m.arg("--model")
    m.reset_scene()

    def rgba(cols):
        return [m.srgb(*c) for c in cols]

    if name == "island":
        # Three objects, not one: the game finds the gates by name to hang
        # room transitions on them, and the terrain and the landmark have
        # different collision stories.
        tv, tf, tc = build_island_terrain()
        m.make_material("terrain")
        m.make_mesh("terrain", tv, tf, "terrain", colors=rgba(tc), smooth=False)

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
