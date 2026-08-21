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

Coordinates here are Blender's: +Z is up. kilnlib's exporter converts to the
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
# A storm sky, not a clear one. Overcast has almost no gradient — the
# interesting variation is horizontal, in the cloud, not vertical — so these
# sit close together and the horizon is the LIGHTEST band because that is
# where the last of the light gets under the cloud base.
ZENITH   = (14, 16, 22)    # cloud base directly overhead, nearly black
MID_SKY  = (22, 26, 34)
HORIZON  = (52, 58, 70)    # ALSO the fog colour and the sea's outer ring
# The moon is BEHIND the cloud: a diffuse bright patch, not a disc, and much
# dimmer than the rock it used to be. It still marks a direction, which is
# what the sea's lane and the key light are aligned to.
MOON     = (96, 102, 116)
MOON_HALO = (58, 64, 78)
STAR     = (40, 46, 58)    # all but extinguished; a hint of break in cloud

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

# The Keys read as the Keys because of the SHELF: a broad band of water so
# shallow that the sand under it comes back up through the colour, ringing
# the island in turquoise before it falls off to blue. pm_world's terrain
# carries that shelf out to R_WATER at -3 m; these are the colours over it.
# Storm water. The shelf still reads lighter than the deep — the sand under
# it does not go away — but the turquoise does: under cloud there is no sun
# to put it there, and a bright tropical shallow would fight the mood the
# fog is building.
SHALLOW  = (74, 96, 104)     # over sand, right off the beach
REEF     = (46, 62, 78)      # the flat further out
DEEP_SEA = (22, 30, 44)
NEAR_SEA = REEF
# Not a moon path any more: a dull sheen where the brightest part of the sky
# reflects. Kept because a completely unbroken sea reads as a plane.
MOON_LANE = (92, 100, 116)

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

def build_skydome(radius=1.0, segments=24, rings=6, star_count=10):
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

    # Bloated and soft: light diffusing through cloud, not a disc.
    core = math.radians(MOON_ANGULAR_RADIUS_DEG * 2.2)
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
    # (kilnlib.py's header states the rule), so a rebuild is byte-identical.
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

def build_sea(inner=55.0, outer=210.0, rings=7, segments=20, uv_tiles=6.0):
    """Radial grid, dense at the centre and coarsening outward.

    Returns (verts, faces, colors, uvs).

    ── Why `outer` is smaller than the far plane ──────────────────────────
    The water used to reach 22,400 units, well past where the fog has fully
    saturated it. Every one of those pixels was drawn — a texture fetch and
    a blend each — to produce exactly the horizon colour the sky dome behind
    it is already painting. Pulling the rim in to 13,400 removes that band
    of pure overdraw and is invisible, because the two colours are the same
    by construction (see PM_ENV_HORIZON in pm_env.h).

    Fill rate, not triangle count, is what the water costs on this console:
    it covers most of the screen. The tessellation came down with the radius
    because the surface is flatter than it was, not to save RSP time.

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

            # Three stops, not two: turquoise over the shelf, reef blue at
            # its edge, then open-ocean blue. The first stop is short — the
            # shelf ends not far past the beach — which is what gives the
            # island a distinct rim of colour rather than a smooth gradient.
            if t < 0.22:
                base = _lerp(SHALLOW, REEF, t / 0.22)
            else:
                base = _lerp(REEF, DEEP_SEA, min(1.0, (t - 0.22) / 0.55))
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


# ── Lightning ──────────────────────────────────────────────────────────
BOLT_VARIANTS = 3
BOLT_SEGMENTS = 17
BOLT_CORE  = (238, 242, 255)
BOLT_EDGE  = (120, 150, 210)


def build_bolt(variant, segments=BOLT_SEGMENTS):
    """One lightning bolt as a flat ribbon. Returns (verts, faces, colors).

    Authored in a UNIT box: x is the jag, z runs 0 (ground) to 1 (cloud),
    and the ribbon faces +Y. pm_env.c scales it to the real strike height
    and yaws it to face the camera, so one mesh serves every strike.

    A ribbon rather than a billboarded line because Tiny3D draws models, not
    immediate-mode geometry — and a model placed by a transform is something
    this engine already does well. Turning it to face the camera each strike
    is one yaw, and a bolt seen edge-on for a few frames is not a bug anyone
    will catch at 60 fps in a storm.

    Colour runs bright core at the top to dimmer at the ground: the channel
    is brightest where the charge comes from, and it gives the eye a
    direction without needing a gradient texture.
    """
    verts, faces, colors = [], [], []
    # Deterministic jag: no `random` anywhere in this pipeline (kilnlib.py).
    # Two incommensurate sines per variant give a path that never repeats
    # over the length of the bolt and differs between variants.
    for i in range(segments + 1):
        t = i / float(segments)
        phase = 3.0 + variant * 2.7
        jag = (math.sin(t * phase * 6.0 + variant * 1.9) * 0.16
               + math.sin(t * phase * 13.0 + variant * 4.1) * 0.085)
        # The strike point is fixed; the spread grows toward the cloud.
        jag *= t
        # A LINE, not a ribbon of triangles. These fractions are multiplied
        # by the strike height (BOLT_TOP, ~140 m), so the first version's
        # 0.012-0.042 came out between 1.7 and 5.9 METRES across — a wedge,
        # not a lightning channel. 0.0015-0.0040 is 20-50 cm, which at N64
        # resolution is the one-to-three pixels a bolt should be.
        w = 0.0015 + 0.0025 * math.sin(t * math.pi)
        col = _lerp(BOLT_EDGE, BOLT_CORE, t)
        verts.append((jag - w, 0.0, t))
        colors.append(col)
        verts.append((jag + w, 0.0, t))
        colors.append(col)
    for i in range(segments):
        a = i * 2
        faces.append((a, a + 1, a + 3, a + 2))
    return verts, faces, colors


# ── Blender entry point ────────────────────────────────────────────────

def main():
    import kilnlib as m

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
    elif name == "storm":
        # Several bolts as named objects in one model, so a strike can pick
        # a different channel each time without a draw-call per variant or a
        # model load per strike.
        for i in range(BOLT_VARIANTS):
            bv, bf, bc = build_bolt(i)
            nm = "bolt_%d" % i
            m.make_material(nm)
            m.make_mesh(nm, bv, bf, nm, colors=rgba(bc), smooth=False)

    else:
        raise SystemExit(
            "pm_env.py: no model '%s' (have: skydome, sea, storm)" % name)

    m.report()
    m.export_gltf(m.arg("--out"))


if __name__ == "__main__":
    # Importable for tests without dragging in bpy.
    if any(a.startswith("--model") for a in sys.argv):
        main()
