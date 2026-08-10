#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""pm_env.py — the moonlit island environment: sky dome and sea.

    blender --background --python pm_env.py -- --model skydome --out sky.gltf

Two models, both vertex-coloured, both built for a night exterior:

  skydome   An upper hemisphere seen from the inside, carrying the night
            gradient, the moon and the stars. Drawn unlit and depth-write-off
            with the camera at its centre, so it is a backdrop rather than
            geometry — it can never be reached, clipped into, or occluded.

  sea       A radial grid centred on the island, dense near the shore and
            coarsening out to the fog horizon. Carries UVs for the scrolling
            foam texture and vertex colours for the water and the moon path.

── Why the horizon colour appears twice ───────────────────────────────────
The dome's lowest ring and the sea's outermost ring are both HORIZON, and the
game sets its fog colour to the same value (pm_env.c). That three-way match is
the whole trick: the sea fades into fog, the fog is the colour of the sky at
the horizon, and the sea's outer edge — which is a hard polygon boundary a few
thousand units out — becomes invisible. Change one and you must change all
three, which is why the constant lives here and pm_env.c quotes it.

── Testable without Blender ───────────────────────────────────────────────
build_skydome() and build_sea() are plain Python returning (verts, faces,
colors[, uvs]) and import no bpy. tools/blender/test_env.py checks their
counts and colour ramps with a bare python3, which is how the same discipline
caught two real bugs in quake_map.py before Blender was ever run.

Coordinates here are Blender's: +Z is up. m64lib's exporter converts to the
engine's Y-up on the way out, the same as every other script in this folder.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ── The palette ────────────────────────────────────────────────────────
# Few, flat, and separated in VALUE rather than hue — the same rule
# _island_colors follows in pm_props.py, and for the same reason: the veil
# collapses hue, so anything that reads only by hue stops reading at all.
#
# These are the colours BEFORE lighting. The sky and moon draw unlit, so what
# is here is what ships; the sea is lit, so moonlight lands on top of it.
ZENITH   = (6, 9, 26)      # deep night blue, straight up
MID_SKY  = (14, 20, 44)
HORIZON  = (38, 48, 74)    # ALSO the fog colour and the sea's outer ring
MOON     = (236, 240, 226) # not pure white: the moon is a rock
MOON_HALO = (92, 104, 132)
STAR     = (176, 188, 214)

# ── The sea's colours are CEILINGS, not water colours ──────────────────
# The sea is drawn texel * vertex-colour, and the foam texture's floor is
# FOAM_FLOOR/255 (~20%, tools/gen_textures.py). So a vertex colour here is
# what a CREST at that point looks like; flat water between crests is that
# colour at a fifth strength. Read them as "moonlit spray", not "sea".
#
# Concretely: DEEP_SEA below lands as roughly (7,11,24) of actual water,
# which is the darker-than-sky value the horizon match wants — sky is what
# the water reflects, and water reflects less than it receives.
FOAM_FLOOR = 0x34 / 255.0  # MUST match tools/gen_textures.py's FOAM_FLOOR

DEEP_SEA = (36, 55, 118)
NEAR_SEA = (64, 94, 188)
MOON_LANE = (198, 214, 244)  # the glitter path running out toward the moon

# Where the moon sits, as a compass direction in the XY plane plus an
# elevation. pm_env.c derives its key light direction from the same two
# numbers, so the terrain is lit from wherever the moon actually is.
MOON_AZIMUTH_DEG = 218.0
MOON_ELEVATION_DEG = 34.0
MOON_ANGULAR_RADIUS_DEG = 3.6


def _moon_dir():
    """Unit vector toward the moon, Blender axes (+Z up)."""
    az = math.radians(MOON_AZIMUTH_DEG)
    el = math.radians(MOON_ELEVATION_DEG)
    return (math.cos(el) * math.cos(az),
            math.cos(el) * math.sin(az),
            math.sin(el))


def _lerp(c0, c1, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(a + (b - a) * t)) for a, b in zip(c0, c1))


# ── Sky ────────────────────────────────────────────────────────────────

def build_skydome(radius=1.0, segments=24, rings=6, star_count=44):
    """Upper hemisphere, inward-facing, plus a moon disc and star quads.

    Returns (verts, faces, colors). `radius` is 1.0 because pm_env.c scales
    the dome at draw time — the mesh is a direction field, not a place.
    """
    verts, faces, colors = [], [], []

    # The dome. Rings run from the horizon (ring 0) to the zenith, so the
    # gradient index is trivially the ring.
    for ring in range(rings + 1):
        v = ring / rings                      # 0 at horizon, 1 at zenith
        phi = v * (math.pi / 2.0)
        z = math.sin(phi) * radius
        r = math.cos(phi) * radius
        # Two-stop ramp: the lower half moves fast (that is where the eye
        # reads "sky meets sea"), the upper half is nearly flat.
        if v < 0.5:
            col = _lerp(HORIZON, MID_SKY, v / 0.5)
        else:
            col = _lerp(MID_SKY, ZENITH, (v - 0.5) / 0.5)
        for seg in range(segments):
            a = 2.0 * math.pi * seg / segments
            verts.append((math.cos(a) * r, math.sin(a) * r, z))
            colors.append(col)

    for ring in range(rings):
        for seg in range(segments):
            nxt = (seg + 1) % segments
            a = ring * segments + seg
            b = ring * segments + nxt
            c = (ring + 1) * segments + nxt
            d = (ring + 1) * segments + seg
            # Wound to be seen from INSIDE — the dome is drawn with
            # CULL_FRONT and the camera is at its centre.
            faces.append((a, b, c, d))

    # ── The moon ────────────────────────────────────────────────────────
    # A disc with a bright core ring and a dim outer ring, so the halo is a
    # vertex-colour gradient rather than a texture or a second pass.
    mx, my, mz = _moon_dir()
    # An orthonormal basis on the sky at the moon's direction.
    up = (0.0, 0.0, 1.0) if abs(mz) < 0.9 else (1.0, 0.0, 0.0)
    ex = (up[1] * mz - up[2] * my, up[2] * mx - up[0] * mz, up[0] * my - up[1] * mx)
    n = math.sqrt(sum(c * c for c in ex)) or 1.0
    ex = tuple(c / n for c in ex)
    ey = (my * ex[2] - mz * ex[1], mz * ex[0] - mx * ex[2], mx * ex[1] - my * ex[0])

    core = math.radians(MOON_ANGULAR_RADIUS_DEG)
    halo = core * 2.6
    # Slightly inside the dome so it can never z-fight with it.
    mr = radius * 0.995
    centre = len(verts)
    verts.append((mx * mr, my * mr, mz * mr))
    colors.append(MOON)

    moon_segments = 12
    for band, (ang, col) in enumerate(((core, MOON), (halo, MOON_HALO))):
        base = len(verts)
        for seg in range(moon_segments):
            t = 2.0 * math.pi * seg / moon_segments
            ox = math.cos(t) * math.sin(ang)
            oy = math.sin(t) * math.sin(ang)
            d = (mx * math.cos(ang) + (ex[0] * ox + ey[0] * oy),
                 my * math.cos(ang) + (ex[1] * ox + ey[1] * oy),
                 mz * math.cos(ang) + (ex[2] * ox + ey[2] * oy))
            ln = math.sqrt(sum(c * c for c in d)) or 1.0
            verts.append(tuple(c / ln * mr for c in d))
            colors.append(col)
        for seg in range(moon_segments):
            nxt = (seg + 1) % moon_segments
            if band == 0:
                faces.append((centre, base + seg, base + nxt))
            else:
                inner = base - moon_segments
                faces.append((inner + seg, base + seg, base + nxt, inner + nxt))

    # ── Stars ───────────────────────────────────────────────────────────
    # Deterministic placement: no random module anywhere in this pipeline
    # (m64lib.py's header states the rule), so a rebuild is byte-identical.
    # A cheap low-discrepancy sequence gives a scatter that does not band.
    GOLDEN = 2.399963229728653
    for i in range(star_count):
        t = (i + 0.5) / star_count
        # Bias away from the horizon: stars there would sit in the fog band
        # and read as noise on the sea.
        elev = math.asin(0.18 + 0.78 * t)
        az = GOLDEN * i
        sx = math.cos(elev) * math.cos(az)
        sy = math.cos(elev) * math.sin(az)
        sz = math.sin(elev)
        # Skip anything that would land on the moon.
        if sx * mx + sy * my + sz * mz > math.cos(halo * 1.5):
            continue
        # Two sizes, so the field has some depth to it.
        size = 0.0075 if (i % 3) else 0.0115
        sr = radius * 0.99
        bx = (-sy, sx, 0.0)
        bn = math.sqrt(bx[0] ** 2 + bx[1] ** 2) or 1.0
        bx = (bx[0] / bn, bx[1] / bn, 0.0)
        by = (sy * bx[2] - sz * bx[1], sz * bx[0] - sx * bx[2], sx * bx[1] - sy * bx[0])
        base = len(verts)
        for dx, dy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = (sx + (bx[0] * dx + by[0] * dy) * size,
                 sy + (bx[1] * dx + by[1] * dy) * size,
                 sz + (bx[2] * dx + by[2] * dy) * size)
            ln = math.sqrt(sum(c * c for c in p)) or 1.0
            verts.append(tuple(c / ln * sr for c in p))
            colors.append(STAR if (i % 3) else MOON_HALO)
        faces.append((base, base + 1, base + 2, base + 3))

    return verts, faces, colors


# ── Sea ────────────────────────────────────────────────────────────────

def build_sea(inner=55.0, outer=350.0, rings=9, segments=28, uv_tiles=9.0):
    """Radial grid, dense at the centre and coarsening outward.

    Returns (verts, faces, colors, uvs).

    ── Why `inner` is well inside the island ──────────────────────────────
    The sea does not stop at the shoreline; it runs on UNDER the island and
    is simply occluded by it. Ending the disc at the island's edge means the
    two meshes meet along a ragged coastline at the same height, which
    z-fights along the whole rim. Starting the water inside the island and
    letting the terrain's underwater skirt (pm_world.py's UNDERWATER_Z) sit
    on top of it makes the waterline an intersection of two solids, which is
    what it is in life and what the depth buffer handles without complaint.

    ── Why the radii are Blender units and not island-widths ──────────────
    The obvious authoring unit for a sea around an island is "island
    half-widths", and that is what this used to use — with pm_env.c scaling
    by PM_ISLAND_HALF_W at draw time. It made the numbers here readable and
    the sea unanimatable: at that scale one exported vertex unit was ~100
    world units, so the swell could only ever displace a vertex by zero or
    by a hundred metres. Rounded to int16, every swell amplitude small
    enough to look like water rounded to nothing.

    So these are Blender units, which the exporter multiplies by 64 to get
    the model's integer coordinates, which pm_env.c then draws at scale 1.
    One vertex unit is one world unit, the swell is expressed in world units
    directly, and the island's half-width is simply 100.1 of these
    (PM_ISLAND_HALF_W / 64).

    The ring spacing is quadratic rather than linear: near water occupies
    most of the screen and needs the vertices, while the outer rings only
    have to reach the fog. Linear spacing spends half the mesh on water the
    fog has already swallowed.
    """
    verts, faces, colors, uvs = [], [], [], []
    mx, my, _ = _moon_dir()
    mlen = math.hypot(mx, my) or 1.0
    mx, my = mx / mlen, my / mlen

    for ring in range(rings + 1):
        t = ring / rings
        r = inner + (outer - inner) * (t * t)
        for seg in range(segments):
            a = 2.0 * math.pi * seg / segments
            cx, cy = math.cos(a), math.sin(a)
            verts.append((cx * r, cy * r, 0.0))
            uvs.append((cx * r * uv_tiles / outer, cy * r * uv_tiles / outer))

            # Water darkens with distance before the fog takes over, so the
            # near water is not the same flat slab as the far water.
            base = _lerp(NEAR_SEA, DEEP_SEA, min(1.0, t * 1.4))
            # The moon path: a lane of brighter water pointing at the moon.
            # Falls off with the angle away from the moon's bearing, and
            # fades in the distance so it does not fight the horizon.
            align = cx * mx + cy * my
            lane = max(0.0, align) ** 14
            lane *= (1.0 - 0.55 * t)
            colors.append(_lerp(base, MOON_LANE, lane))

    for ring in range(rings):
        for seg in range(segments):
            nxt = (seg + 1) % segments
            a = ring * segments + seg
            b = ring * segments + nxt
            c = (ring + 1) * segments + nxt
            d = (ring + 1) * segments + seg
            faces.append((a, d, c, b))

    return verts, faces, colors, uvs


# ── Blender entry point ────────────────────────────────────────────────

def main():
    import m64lib as m

    name = m.arg("--model")
    m.reset_scene()

    # The builders above deal in readable 0-255 sRGB triples — the same
    # numbers pm_env.h quotes and test_env.py checks. Blender's COLOR_0 wants
    # LINEAR FLOAT RGBA, so the conversion happens here, at the one boundary
    # where bpy is already in the room. Doing it inside the builders would
    # make the tests either import bpy or assert against gamma-encoded floats,
    # and both are worse than one map().
    def rgba(cols):
        return [m.srgb(*c) for c in cols]

    if name == "skydome":
        verts, faces, colors = build_skydome()
        m.make_material("sky")
        m.make_mesh("sky", verts, faces, "sky", colors=rgba(colors),
                    smooth=False)
    elif name == "sea":
        verts, faces, colors, uvs = build_sea()
        m.make_material("water")
        m.make_mesh("water", verts, faces, "water",
                    colors=rgba(colors), uvs=uvs, smooth=False)
    else:
        raise SystemExit("pm_env.py: no model '%s' (have: skydome, sea)" % name)

    m.report()
    m.export_gltf(m.arg("--out"))


if __name__ == "__main__":
    # Importable for tests without dragging in bpy.
    if any(a.startswith("--model") for a in sys.argv):
        main()
