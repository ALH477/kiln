# SPDX-License-Identifier: MIT
"""interceptor.py — a low-poly starfighter, the hero-prop reference.

    blender --background --factory-startup -noaudio \
        --python tools/blender/interceptor.py -- --model interceptor \
        --out build/interceptor

Everything else authored in this directory is a test: models.py isolates one
renderer bug per shape, goblin.py exists to exercise skinning. Nothing was a
model built the way a game's actual content gets built — one silhouette,
assembled from several parts, shaded entirely by vertex colour, sized and
budgeted for a scene that also has to run at 60 Hz. This is that reference,
and it is what kilnlib's loft()/slab() were added for.

── Orientation ────────────────────────────────────────────────────────────
Nose along Blender +Y, which export_yup turns into engine -Z: the direction
glTF (and therefore Tiny3D) calls forward, so the ship flies the way a camera
looking down -Z expects without a correction matrix at draw time. Up is
Blender +Z -> engine +Y. See models.py's `axes` model if that mapping is ever
in doubt.

The whole ship is ~5.7 long and ~5.4 across, standing at the origin. At the
default --base-scale=64 that is ~365 Tiny3D units — the same size class as
goblin.py's 2.3-unit character, and well inside the int16 vertex range.

── No texture, on purpose ─────────────────────────────────────────────────
Every surface is COLOR_0 through the `shade` preset, which is exactly the
combiner kiln_scene_begin() already sets — so this model composes with
hand-built geometry in the same pass, costs no TMEM, and needs no tile setup.
Panel lines and insignia would need a texture and a UV unwrap; the colour
here does the work instead, which is the trade N64-era models generally made.

── Interpenetration is the technique ──────────────────────────────────────
The canopy sinks into the hull, the nacelles into the wings, the fins into
the tail. No boolean is performed and none should be: overlapping closed
convex solids cost nothing extra on a Z-buffered renderer, whereas a CSG
union would weld them into one concave mesh whose caps loft()/slab() can no
longer fan, and whose vertex count would climb past the 70-vertex chunk
threshold for no visible gain.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import kilnlib as m  # noqa: E402

# ── palette ────────────────────────────────────────────────────────────────
# Cold hull, one warm accent, one hot exhaust. Two families and no more: with
# no texture and no specular term, colour is the only thing separating the
# parts, and a third hue reads as a mistake rather than as detail.
HULL_TOP = m.srgb(158, 174, 196)     # lit upper surfaces
HULL_MID = m.srgb(96, 110, 133)
HULL_BELLY = m.srgb(40, 48, 63)      # deep shadow underside
ACCENT = m.srgb(232, 84, 58)         # nose flash, fin leading edges
CANOPY_GLASS = m.srgb(58, 208, 214)  # teal, reads as glass against slate
CANOPY_RIM = m.srgb(20, 44, 58)
ENGINE_CASE = m.srgb(74, 84, 104)
ENGINE_HOT = m.srgb(255, 176, 72)    # exhaust throat
ENGINE_CORE = m.srgb(255, 246, 214)  # the one near-white in the model
GUN_BODY = m.srgb(62, 70, 88)
GUN_TIP = m.srgb(255, 120, 66)


def _t(value, lo, hi):
    """0..1 ramp, clamped. Every gradient below is one of these."""
    return max(0.0, min(1.0, (value - lo) / (hi - lo)))


# ── hull ───────────────────────────────────────────────────────────────────
# One cross-section, scaled per station. Points run in increasing atan2(z, x)
# — loft()'s winding rule for a +Y sweep, and the reason the belly points come
# last. Eight points: enough for a chined, angular silhouette, few enough that
# six stations still fit one 70-vertex chunk (6*8 + 2 caps = 50).
HULL_PROFILE = [
    (1.00, 0.00),    # chine, the widest line down each side
    (0.74, 0.56),
    (0.30, 0.86),    # spine
    (-0.30, 0.86),
    (-0.74, 0.56),
    (-1.00, 0.00),
    (-0.56, -0.46),  # belly, deliberately narrower than the top
    (0.56, -0.46),
]

# (y, half-width, half-height, centre z). Nose never reaches zero width: a
# collapsed ring makes degenerate quads, which make_mesh's validate() rejects.
# Five stations, not six: each station costs 16 tris (an octagonal ring), so
# dropping one buys 16 against the 360-tris budget without reshaping the
# silhouette much — the drop here is at y=1.00, the tail-taper inflection,
# which the engines on top of it hide anyway.
HULL_STATIONS = [
    (-2.40, 0.56, 0.44, 0.00),
    (-1.40, 0.74, 0.57, 0.02),
    (-0.20, 0.82, 0.60, 0.05),
    (2.10, 0.42, 0.28, -0.04),
    (2.85, 0.09, 0.07, -0.10),
]


def _section(profile, y, sx, sz, cz, cx=0.0):
    return [(cx + x * sx, y, cz + z * sz) for x, z in profile]


def hull_color(v):
    """Ramps are fitted to the geometry that actually exists, not to round
    numbers: the belly only reaches z=-0.25, so a ramp starting at -0.45 would
    put the darkest colour somewhere no vertex is, and the hull would come out
    uniformly mid-grey with the shading doing all the work. With no texture
    and one light, that flatness is the whole model."""
    x, y, z = v
    c = m.mix(HULL_BELLY, HULL_MID, _t(z, -0.26, 0.10))
    c = m.mix(c, HULL_TOP, _t(z, 0.10, 0.55))
    return m.mix(c, ACCENT, _t(y, 2.05, 2.85))    # nose flash


def build_hull():
    verts, faces = m.loft([_section(HULL_PROFILE, *s) for s in HULL_STATIONS])
    m.make_mesh("Hull", verts, faces, "HullMat",
                colors=[hull_color(v) for v in verts])


# ── canopy ─────────────────────────────────────────────────────────────────
# A teardrop: round over the top, drawn to a point underneath so it sits into
# the hull spine without a visible seam where the two solids cross.
CANOPY_PROFILE = [
    (1.00, 0.00), (0.62, 0.78), (0.00, 1.00),
    (-0.62, 0.78), (-1.00, 0.00), (0.00, -0.62),
]

CANOPY_STATIONS = [
    (0.30, 0.30, 0.11, 0.50),
    (1.00, 0.38, 0.22, 0.52),
    (1.85, 0.15, 0.07, 0.44),
]


def build_canopy():
    verts, faces = m.loft([_section(CANOPY_PROFILE, *s)
                           for s in CANOPY_STATIONS])
    colors = [m.mix(CANOPY_RIM, CANOPY_GLASS, _t(v[2], 0.42, 0.68))
              for v in verts]
    m.make_mesh("Canopy", verts, faces, "CanopyMat", colors=colors, smooth=True)


# ── wings ──────────────────────────────────────────────────────────────────
# Double delta with drooping tips. slab() carries a centre and a half-thickness
# per point, so the taper and the anhedral are both in the point list rather
# than in a modifier: root is thick and level, tip is thin and 0.22 low.
#
# CCW in the XY projection, which for the right-hand wing means root leading
# edge -> root trailing -> outward along the trailing edge -> tip -> back
# inward along the leading edge.
#
# The mid-span pair is not decoration. Two things need it:
#
#   * COLOUR. On a four-corner wing the tip accent has nothing to interpolate
#     against but the root, so it washes the entire panel pink — the first
#     render of this model was a salmon aeroplane. A vertex at mid-span is
#     what confines the flash to the outer third. With no texture, vertex
#     placement IS colour resolution.
#   * SHAPE. slab() fans its caps from a centroid, so the planform has to stay
#     convex: the mid leading-edge point must sit FORWARD of the straight
#     root-tip line (y=0.21 at x=1.75) and the mid trailing-edge point AFT of
#     its own (y=-1.59). Pulling them the other way to get a swept-in "kink"
#     silently produces overlapping cap triangles. Obeying it is what makes
#     this a double delta rather than a plain one, which is the better
#     silhouette anyway.
WING = [
    (0.55, 1.05, 0.00, 0.16),    # root, leading edge
    (0.55, -1.95, 0.00, 0.13),   # root, trailing edge
    (1.75, -1.72, -0.06, 0.10),  # mid, trailing edge  (aft of the chord line)
    (2.55, -1.35, -0.22, 0.05),  # tip, trailing edge
    (2.55, -0.35, -0.22, 0.05),  # tip, leading edge
    (1.75, 0.55, -0.06, 0.10),   # mid, leading edge   (forward of it)
]


def wing_color(v, top):
    """slab() emits every bottom vertex before every top one, so which surface
    a vertex belongs to is known exactly — better than inferring it from z,
    which the drooping tip makes ambiguous (the tip's UPPER surface sits lower
    than the root's LOWER one)."""
    span = _t(abs(v[0]), 0.55, 2.55)
    c = (m.mix(HULL_TOP, HULL_MID, span) if top
         else m.mix(HULL_BELLY, HULL_MID, span * 0.4))
    # Outer panel only, and not all the way to full accent: the tip is a
    # flash on a grey aeroplane, not a red one.
    return m.mix(c, ACCENT, _t(abs(v[0]), 1.75, 2.55) * 0.85)


def build_wings():
    for side, points in (("R", WING), ("L", m.mirror_x(WING))):
        verts, faces = m.slab(points)
        colors = [wing_color(v, i >= len(points)) for i, v in enumerate(verts)]
        m.make_mesh(f"Wing{side}", verts, faces, "HullMat", colors=colors)


# ── wingtip cannons ────────────────────────────────────────────────────────
GUN_PROFILE = [(1.00, 0.00), (0.50, 0.87), (-0.50, 0.87),
               (-1.00, 0.00), (-0.50, -0.87), (0.50, -0.87)]

GUN_STATIONS = [
    (-1.05, 0.12, 0.12, -0.22),
    (0.35, 0.15, 0.15, -0.22),
    (1.10, 0.07, 0.07, -0.22),
]


def build_guns():
    for side, sign in (("R", 1.0), ("L", -1.0)):
        verts, faces = m.loft([_section(GUN_PROFILE, y, sx, sz, cz,
                                        cx=sign * 2.35)
                               for y, sx, sz, cz in GUN_STATIONS])
        colors = [m.mix(GUN_BODY, GUN_TIP, _t(v[1], 0.5, 1.1)) for v in verts]
        m.make_mesh(f"Gun{side}", verts, faces, "HullMat", colors=colors)


# ── engines ────────────────────────────────────────────────────────────────
# Hexagonal, not round: at this radius on a 320x240 framebuffer the flat
# facets are what makes the nacelle read as machined rather than as a blob,
# and six segments cost half of twelve.
ENGINE_PROFILE = GUN_PROFILE

# The throat station exists only so the glow has somewhere to stop. Without
# it the nearest vertex forward is 0.65 away and the exhaust colour
# interpolates over the whole rear half of the nacelle — which renders as a
# cream-coloured blob, not as an engine.
ENGINE_STATIONS = [
    (-2.55, 0.30, 0.30, -0.06),   # exhaust mouth
    (-2.32, 0.32, 0.32, -0.06),   # throat
    (-1.90, 0.35, 0.35, -0.06),
    (-0.90, 0.30, 0.30, -0.06),
]


def build_engines():
    for side, sign in (("R", 1.0), ("L", -1.0)):
        cx = sign * 0.80
        verts, faces = m.loft([_section(ENGINE_PROFILE, y, sx, sz, cz, cx=cx)
                               for y, sx, sz, cz in ENGINE_STATIONS])
        # Two ramps, radial and axial, both clamped. The axial one fades
        # ENGINE_CASE in at the THIRD station (-1.90), not the throat, so the
        # throat ring and the rear half of the nacelle stays at the hot
        # colour — without an emissive term in the combiner, a glow has to be
        # the only colour on those surfaces, not a tint over the case grey.
        # The radial ramp widens way out (0.40) so the cap's white-hot core
        # bleeds onto the throat sidewall, which is what you see from the
        # side and from above: the only ENGINE_CORE on the visible silhouette.
        colors = []
        for v in verts:
            radial = ((v[0] - cx) ** 2 + (v[2] + 0.06) ** 2) ** 0.5
            hot = m.mix(ENGINE_CORE, ENGINE_HOT, _t(radial, 0.04, 0.40))
            colors.append(m.mix(hot, ENGINE_CASE, _t(v[1], -1.90, -0.90)))
        m.make_mesh(f"Engine{side}", verts, faces, "EngineMat", colors=colors,
                    smooth=True)


# ── tail fins ──────────────────────────────────────────────────────────────
# Canted outward, so from behind they read as a V rather than as two slabs.
#
# slab() extrudes along Z, which is the wrong axis for something vertical, so
# the plate is authored lying down (x = height, y = chord) and rotated onto
# its edge by _upright(). Rotate-and-shear, det +1 for BOTH signs — a plain
# axis swap, or mirror_x() on a plate whose x means height, would be a
# reflection: every face inside out, or in mirror_x()'s case a fin pointing
# into the floor. That is the failure tools/blender/test_prims.py exists for.
FIN = [
    (0.00, 0.30, 0.0, 0.07),    # root, leading edge
    (0.00, -0.95, 0.0, 0.07),   # root, trailing edge
    (1.00, -0.88, 0.0, 0.04),   # tip, trailing edge
    (1.00, -0.28, 0.0, 0.04),   # tip, leading edge
]

FIN_CANT = 0.42     # outward lean per unit of height
FIN_BASE = (0.46, -1.55, 0.28)   # right-hand root, in ship space


def _upright(verts, sign):
    """(x=height, y=chord, z=thickness) -> ship space, then cant and place."""
    out = []
    for x, y, z in verts:
        height = x
        out.append((sign * (FIN_BASE[0] + FIN_CANT * height) - z,
                    FIN_BASE[1] + y,
                    FIN_BASE[2] + height))
    return out


def build_fins():
    for side, sign in (("R", 1.0), ("L", -1.0)):
        verts, faces = m.slab(FIN)
        verts = _upright(verts, sign)
        colors = [m.mix(HULL_MID, ACCENT, _t(v[2], 0.9, 1.5)) for v in verts]
        m.make_mesh(f"Fin{side}", verts, faces, "HullMat", colors=colors)


def build_interceptor():
    build_hull()
    build_canopy()
    build_wings()
    build_guns()
    build_engines()
    build_fins()


def main():
    name = m.arg("--model", "interceptor")
    if name != "interceptor":
        raise SystemExit(f"interceptor.py: no model '{name}'")

    m.reset_scene()
    build_interceptor()
    # Sized against models.py's `torus` (360) — a hero prop may cost about
    # what the most expensive test shape costs, and no more, because a scene
    # has to fit several of these plus a room.
    m.report(max_tris=360)
    m.export_gltf(m.arg("--out"))


main()
