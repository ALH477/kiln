# SPDX-License-Identifier: MPL-2.0
"""vehicles.py — the goblins' rides: a go-kart and a motorcycle.

    blender --background --factory-startup -noaudio \
        --python tools/blender/vehicles.py -- --model gokart \
        --out build/gokart

    blender --background --factory-startup -noaudio \
        --python tools/blender/vehicles.py -- --model bike \
        --out build/bike

Two hero props in the sense interceptor.py established: one silhouette each,
assembled from interpenetrating solids, shaded entirely by COLOR_0 through the
`shade` combiner kiln_scene_begin() already sets, no texture and no TMEM.

── Why one file for two models ────────────────────────────────────────────
They share the parts that are hard to get right — the wheel (a cylinder laid
on its side, with the tyre/rim colour split falling out of cylinder()'s own
vertex order), the swept exhaust, the fender over a wheel — and those helpers
are where the sign errors live. Two files would mean two copies of the wheel
builder and, on past form, one of them subtly inside out. The models
themselves are only their part lists.

── Orientation and scale ──────────────────────────────────────────────────
Nose along Blender +Y (engine -Z after export_yup: forward), up +Z, and z=0 is
the GROUND, not the centre of mass — a vehicle is placed by where its tyres
touch, so the model's origin is where the game will put it. Both are sized for
goblin.py's ~2.3-unit character to sit on: the kart measures 3.0 long, 1.7
across the rear track and 0.9 tall; the bike 2.9 long, 0.9 across the bars and
1.2 to the bar tops. Those are the numbers tools/blender/test_vehicles.py
prints, not remembered ones — it reports the assembled bounding box precisely
so this paragraph cannot drift away from the geometry.

── The accent colour is an argument ───────────────────────────────────────
`--accent RRGGBB` tints the bodywork only; tyres, chrome and the engine stay
put. Ganja Goblin gives each seat a colour (gg_player_tint), and four karts
that differ only in bodywork is the cheapest way to tell four identical
vehicles apart at 320x240 — cheaper than four textures, and it needs no
runtime support at all beyond loading a different .t3dm. Building all four is
four derivations sharing one script; the default is the house green.

── Tri budget, and the weld ───────────────────────────────────────────────
Per-model, not shared: 900 for the kart (it uses 708) and 980 for the bike
(804), against interceptor.py's 360. One number for both would have to be the
larger of the two, which stops being a budget for the kart.

The ceilings have been raised three times, deliberately, and it is worth
recording what bought each increase rather than letting it read as drift:

  460 -> 540   build_wheel's hub caps, 80 tris a vehicle. Before them the
               tyres rendered as featureless grey gradient discs (see that
               function's comment for why a colour ramp cannot fix it).
  540 -> 640   the rider station: the kart's pedals, and the bike's forward
               pegs, peg arms and bar risers. Every one exists so that
               goblin.py's riding animations land on a control rather than
               in mid-air. See rider.py.
  640 -> now   detail: 12-sided wheels, 6-sided tubes, 8-point body sections,
               7-step fenders.

That last increase is paid for on the axis that actually matters. _mesh()
welds every part to the console's 1/64 vertex grid, and the reclaim is large:

    go-kart     624 -> 396 verts    708 tris
    motorcycle  724 -> 458 verts    804 tris

36% fewer vertices for MORE triangles. On this console that is the better
trade by a distance — the RSP transforms per vertex and Tiny3D chunks a mesh
every 70 of them, so vertices buy transform time and chunk boundaries while
triangles mostly buy RDP fill, which at 320x240 is not the bottleneck.

A goblin (~870) on a bike (804) is ~1700 tris per player. Four in one frame
is ~6700, which is the number to watch if the board camera ever frames every
player's vehicle at once — that is a real budget now, not a rounding error,
and it is the case for kiln_lod on the far tokens.

── Checked before Blender starts ──────────────────────────────────────────
tools/blender/test_vehicles.py runs every builder here on the host with a
stubbed bpy and checks each part's signed volume — the property that says a
closed surface is oriented outward. It caught two real inversions while these
were being written (a loft station list running -Y, and two slab polygons
wound clockwise), neither of which raises an error anywhere in the pipeline:
they convert cleanly and then vanish under the RDP's back-face culling.
"""

import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import kilnlib as m  # noqa: E402
import rider  # noqa: E402

# ── body scale ─────────────────────────────────────────────────────────────
# Both vehicles are AUTHORED at a convenient size and drawn 1.45x larger.
#
# The first build sized them by eye and got it badly wrong: the go-kart came
# out 3.0 long and 0.85 tall, and a goblin is 2.3 tall, so the render of him
# sitting in it showed a character more than twice the height of his own
# vehicle. A real kart is about 1.75 times its driver's height end to end;
# these are now about 1.7, which for a party game reads as chunky-but-sane
# rather than as a toy under a giant.
#
# Scaling here rather than by editing every literal is what makes it one
# number to change. The catch is the parts placed from rider.py's station —
# the steering wheel, the pedals, the bars, the pegs. Those are in the
# RIDER's units and must not move, so _station() divides them back into
# authored units on the way in, and _mesh() multiplies them out again. The
# net effect is that the body grows around a rider station that stays put,
# which is exactly the property the derived-controls design was for.
BODY_SCALE = 1.45


WELD = {"verts_in": 0, "verts_out": 0, "tris_in": 0, "tris_out": 0,
        "dropped": 0}

# ── the vehicles are RIGGED ────────────────────────────────────────────────
# They used to be one static mesh each, and a driving animation built on that
# can only ever animate the rider: the goblin leans into a corner while his
# kart sits under him like a table, and the wheels do not turn at all. At any
# speed a player would call driving, static wheels are the first thing the
# eye catches.
#
# So every part is bound to a bone, exactly as rigidly as the goblin's are —
# one bone per vertex, no weights — and _mesh() is where that binding is
# recorded. Parts accumulate here and main() hands the whole list to
# make_skinned_mesh once.
#
# The rigs are deliberately small. A kart is a body, a front axle that
# steers, four wheels that spin and a steering wheel that turns; a bike is a
# body, a fork assembly that steers, and two wheels. Nothing deforms, so
# nothing needs more.
PARTS = []
_BONE = ["body"]


def _mesh(name, verts, faces, material, colors=None, smooth=False, bone=None):
    """Scale to body size, weld to the console's vertex grid, then build.

    Every builder below goes through here; nothing calls m.make_mesh
    directly. Scaling happens BEFORE the weld, and that order is the whole
    reason this works: the grid is 1/64 of a FINAL unit, so quantising the
    authored coordinates and then multiplying by 1.45 would land every
    vertex between grid points and gltf_to_t3d would round them all a second
    time, undoing the snap. Snap last, in the space the console sees.
    """
    scaled = [(v[0] * BODY_SCALE, v[1] * BODY_SCALE, v[2] * BODY_SCALE)
              for v in verts]
    w_verts, w_faces, w_colors, st = m.weld(scaled, faces, colors)
    for k in WELD:
        WELD[k] += st[k]
    PARTS.append((bone or _BONE[0], w_verts, w_faces, w_colors))
    return None


def _on(bone):
    """Bind every part built until the next _on() to `bone`.

    A positional bone argument on _mesh would mean threading it through
    build_wheel, build_fender and build_pipe as well, which are called from
    both vehicles with different bones each time — this is the same
    information with one place to set it and no signature churn.
    """
    _BONE[0] = bone


def _S(p):
    """A point in authored units, expressed in the final scaled space the
    bones and the welded meshes both live in."""
    return tuple(c * BODY_SCALE for c in p)


def _station(points):
    """Rider-station points, expressed in authored (pre-scale) units."""
    return [tuple(c / BODY_SCALE for c in p) for p in points]


# ── palette ────────────────────────────────────────────────────────────────
# Three families: rubber/chrome (achromatic), the accent (bodywork), and one
# hot colour for anything that burns. interceptor.py's rule — two families and
# one accent — with a third permitted here because a vehicle genuinely has
# three materials and reading it as one lump is the failure mode.
TYRE_OUT = m.srgb(26, 26, 32)        # tread, the darkest thing in the model
TYRE_IN = m.srgb(52, 52, 62)
RIM = m.srgb(198, 204, 216)          # bare metal
RIM_HUB = m.srgb(240, 244, 252)
CHROME = m.srgb(176, 184, 200)
CHROME_DARK = m.srgb(88, 94, 110)
ENGINE_CASE = m.srgb(70, 76, 92)
ENGINE_FIN = m.srgb(120, 128, 148)
EXHAUST_HOT = m.srgb(255, 150, 62)   # the pipe mouth only
SEAT_DARK = m.srgb(38, 30, 44)
SEAT_LIT = m.srgb(74, 60, 84)
GRIP = m.srgb(44, 36, 40)
LAMP = m.srgb(255, 246, 206)         # the one near-white
LAMP_RIM = m.srgb(120, 116, 100)

# Default bodywork accent: the engine's house green, so an untinted build
# still looks like it belongs to this game.
ACCENT = m.srgb(0, 214, 108)
ACCENT_DARK = m.srgb(0, 96, 56)      # shadowed bodywork
ACCENT_LIT = m.srgb(150, 255, 200)   # top highlight


def _t(value, lo, hi):
    """0..1 ramp, clamped. Every gradient below is one of these."""
    return max(0.0, min(1.0, (value - lo) / (hi - lo)))


def set_accent(hexstr):
    """Recompute the three bodywork colours from one hex triple.

    The dark and lit variants are derived rather than passed: a caller
    supplying three colours can pick a set that has no contrast at all, and
    the whole reason the bodywork reads as a shape without a texture is the
    ramp between them.
    """
    global ACCENT, ACCENT_DARK, ACCENT_LIT
    h = hexstr.lstrip("#")
    if len(h) != 6:
        raise SystemExit(f"vehicles.py: --accent wants RRGGBB, got '{hexstr}'")
    r, g, b = (int(h[i:i + 2], 16) for i in (0, 2, 4))
    ACCENT = m.srgb(r, g, b)
    ACCENT_DARK = m.srgb(int(r * 0.42), int(g * 0.42), int(b * 0.42))
    ACCENT_LIT = m.srgb(min(255, int(r * 0.45 + 140)),
                        min(255, int(g * 0.45 + 140)),
                        min(255, int(b * 0.45 + 140)))


def body_color(v, lo, hi):
    """The bodywork ramp, by height. Both vehicles use it, which is what makes
    them look like they came out of the same shop."""
    c = m.mix(ACCENT_DARK, ACCENT, _t(v[2], lo, (lo + hi) * 0.5))
    return m.mix(c, ACCENT_LIT, _t(v[2], (lo + hi) * 0.5, hi))


# ── shared parts ───────────────────────────────────────────────────────────

def build_wheel(name, cx, cy, radius, width, segments=12, hub_sides=(1, -1)):
    """A wheel: a tyre cylinder laid on its side, plus a hub cap disc.

    ── Why the hub is a second solid ──────────────────────────────────────
    The obvious cheap wheel is one cylinder coloured by distance from the
    axle: bright at the cap centre, black at the rim, hub for free. It does
    not work, and the reason is worth writing down because it looks like it
    should. cylinder()'s cap is a fan whose ONLY vertices are the centre
    (r=0) and the rim (r=R) — there is nothing in between for a ramp to bend.
    Whatever the ramp constants say, the face interpolates linearly from
    centre colour to rim colour across its whole radius, so the wheel renders
    as a big smooth grey gradient disc. Rendered, that reads as a blob, not
    as a tyre; it was the single worst thing about the first version of both
    of these models.

    So the tyre's cap is coloured FLAT dark — a black octagon, which is the
    silhouette a tyre should have — and the hub is its own small disc sitting
    just proud of the face. 20 tris per side, and it is the detail that makes
    a wheel read at 320x240.

    `hub_sides` says which faces get one: (1, -1) for both, (1,) or (-1,) for
    the outboard face only, which is all a kart's wheels ever show.

    rotated() is a proper rotation, so the point list is untouched and the
    winding survives — see tools/blender/test_prims.py's "cylinder, axle
    along X".
    """
    verts, faces = m.cylinder(radius, width, segments, cz=-width / 2)
    verts = m.rotated(verts, (0, 1, 0), 90)
    verts = m.translated(verts, dx=cx, dy=cy, dz=radius)
    # Tread darkest, sidewall a touch lighter — the only variation the tyre
    # gets, and enough that the two faces do not merge with the tread.
    colors = [TYRE_OUT if abs(v[0] - cx) < width * 0.49 else TYRE_IN
              for v in verts]
    _mesh(name, verts, faces, "WheelMat", colors=colors)

    hub_r = radius * 0.44
    for sgn in hub_sides:
        hv, hf = m.cylinder(hub_r, 0.035, 7, cz=0.0)
        hv = m.rotated(hv, (0, 1, 0), 90 * sgn)
        hv = m.translated(hv, dx=cx + sgn * width / 2, dy=cy, dz=radius)
        hc = [m.mix(RIM_HUB, RIM,
                    _t(math.hypot(v[1] - cy, v[2] - radius), 0.0, hub_r))
              for v in hv]
        _mesh(f"{name}Hub{'R' if sgn > 0 else 'L'}", hv, hf, "WheelMat",
                    colors=hc)


def build_fender(name, cx, cy, wheel_r, width, start_deg, end_deg,
                 gap=0.07, steps=7):
    """An arc of bodywork over a wheel.

    The section is a flat rectangle rather than a curved shell: a shell would
    need either a concave cross-section (which loft's fan caps cannot close)
    or twice the rings, and at this size the flat band reads as a mudguard
    either way. `gap` is the clearance over the tyre — without it the fender
    z-fights the wheel along its whole length, which on hardware flickers.
    """
    path = m.arc_path((cx, cy, wheel_r), wheel_r + gap,
                      start_deg, end_deg, steps, plane="yz")
    half_w, half_t = width / 2, 0.035
    # `up` is the WHEEL AXLE, not world up, and this is the one place in the
    # file where that distinction is load-bearing. sweep resolves roll with
    # U = T x up, so a fender arc that passes through vertical — every rear
    # fender does — has its tangent momentarily parallel to world +Z, and U
    # flips sign from one station to the next. The band then crosses over
    # itself: half the solid inside out, no error raised. Rolling about the
    # axle instead keeps T perpendicular to `up` for the whole arc by
    # construction, so it cannot degenerate at any angle.
    #
    # It also swaps what the section's axes mean: with up = +X, u runs
    # RADIALLY (out from the axle) and v runs across the bike.
    section = [(half_t, -half_w), (half_t, half_w),
               (-half_t, half_w), (-half_t, -half_w)]
    verts, faces = m.sweep(path, section, up=(1.0, 0.0, 0.0))
    colors = [body_color(v, wheel_r * 0.6, wheel_r + gap + 0.1) for v in verts]
    _mesh(name, verts, faces, "BodyMat", colors=colors)


def build_pipe(name, path, radius, hot_from=None, segments=6):
    """An exhaust: a swept tube whose LAST station is the hot one.

    `hot_from` is a path index; every vertex from that ring on ramps toward
    EXHAUST_HOT. Keyed to the ring index rather than to a world position
    because the pipe curves — a positional ramp on a pipe that doubles back
    lights up the wrong end, which is how the first version of this came out.
    """
    section = [(radius * math.cos(2 * math.pi * i / segments),
                radius * math.sin(2 * math.pi * i / segments))
               for i in range(segments)]
    verts, faces = m.sweep(path, section)

    colors = []
    n_path = len(path)
    n_ring_verts = n_path * segments
    hot_at = n_path - 1 if hot_from is None else hot_from
    for i, _v in enumerate(verts):
        if i < n_ring_verts:
            ring = i // segments
        else:
            # loft appends the start cap's centre first, then the end cap's.
            # Clamping both to the last ring would light the buried inlet up
            # as brightly as the mouth.
            ring = 0 if i == n_ring_verts else n_path - 1
        c = m.mix(CHROME_DARK, CHROME, _t(ring, 0, max(1, hot_at - 1)))
        colors.append(m.mix(c, EXHAUST_HOT, _t(ring, hot_at - 0.5, n_path - 1)))
    _mesh(name, verts, faces, "PipeMat", colors=colors, smooth=True)


# ══ GO-KART ════════════════════════════════════════════════════════════════
#
# A kart is a floor pan with the driver sitting IN it, side pods either side,
# a pointed nose, an engine hung off the right, and four wheels outside the
# body — that last one is the whole silhouette, and it is why the pods stop
# short of the tyres instead of enclosing them.

# Wheel sizes are the proportion that decides whether this reads as a kart or
# as a beach buggy, and the first pass got it wrong: at 0.36 rear radius on a
# 3.0-long body the tyres were nearly half the vehicle's height and swallowed
# the silhouette. A real kart is about five wheel-diameters long. These are.
KART_FRONT_R = 0.25
KART_REAR_R = 0.30
KART_TRACK_F = 0.58      # half-track, front
KART_TRACK_R = 0.66      # wider at the back, as karts are
KART_WHEELBASE_F = 0.92
KART_WHEELBASE_R = -0.88

# Floor pan, CCW in the XY projection (slab's rule), pinched at the nose and
# widest at the seat. Convexity is not optional — slab fans its caps from a
# centroid, so a concave "waist" between the pods would produce overlapping
# cap triangles rather than a shape.
#
# The rear corners reach out to x=0.50, close to the rear tyres' inner faces
# at 0.51. That is free (the pan is one slab either way) and it is what stops
# the rear wheels reading as two loose discs parked beside the kart.
KART_PAN = [
    (0.44, 0.55, 0.20, 0.035),
    (0.30, 1.15, 0.20, 0.030),
    (-0.30, 1.15, 0.20, 0.030),
    (-0.44, 0.55, 0.20, 0.035),
    (-0.56, -0.30, 0.20, 0.040),
    (-0.50, -1.05, 0.20, 0.040),
    (0.50, -1.05, 0.20, 0.040),
    (0.56, -0.30, 0.20, 0.040),
]


def build_kart_pan():
    verts, faces = m.slab(KART_PAN)
    colors = [m.mix(ACCENT_DARK, ACCENT, _t(v[2], 0.16, 0.24)) for v in verts]
    _mesh("KartPan", verts, faces, "BodyMat", colors=colors)


# Nose cone. Six-point section so the fairing is chined rather than round —
# the same reasoning interceptor.py gives for hexagonal nacelles: at this size
# facets read as moulded plastic and a smooth tube reads as a blob.
KART_NOSE_PROFILE = [
    (1.00, -0.30), (0.92, 0.30), (0.60, 0.75), (0.00, 0.92),
    (-0.60, 0.75), (-0.92, 0.30), (-1.00, -0.30), (0.00, -0.75),
]

KART_NOSE_STATIONS = [
    # (y, half-width, half-height, centre z)
    # Taller and higher than the first pass, which put the nose at the same
    # height as the pods and let it disappear behind the front tyres. A kart's
    # nose cone is the tallest thing forward of the driver; it has to sit
    # above the pod line to be that.
    (0.98, 0.42, 0.19, 0.34),
    (1.34, 0.38, 0.17, 0.33),
    (1.58, 0.17, 0.08, 0.28),
]


def _section(profile, y, sx, sz, cz, cx=0.0):
    return [(cx + x * sx, y, cz + z * sz) for x, z in profile]


def build_kart_nose():
    verts, faces = m.loft([_section(KART_NOSE_PROFILE, *s)
                           for s in KART_NOSE_STATIONS])
    colors = [body_color(v, 0.17, 0.40) for v in verts]
    _mesh("KartNose", verts, faces, "BodyMat", colors=colors)


# Side pods. Authored for the right side and mirrored, which is what mirror_x
# is for — negating x alone would leave the left pod inside out.
#
# Lower and shorter than the first pass. At 0.34 centre and 0.13 half-height
# they stood taller than the seat back and turned the kart into a green box
# with a driver hidden inside it; a pod is a fairing over the side rails, and
# it should sit BELOW the shoulder line so the goblin in the seat is the
# tallest thing on the vehicle.
KART_POD = [
    (0.50, 0.54, 0.28, 0.075),
    (0.50, -0.34, 0.27, 0.095),
    (0.72, -0.28, 0.25, 0.085),
    (0.72, 0.46, 0.26, 0.065),
]


def build_kart_pods():
    for side, points in (("R", KART_POD), ("L", m.mirror_x(KART_POD))):
        verts, faces = m.slab(points)
        colors = [body_color(v, 0.18, 0.46) for v in verts]
        _mesh(f"KartPod{side}", verts, faces, "BodyMat", colors=colors)


def build_kart_seat():
    # Base: a plain wedge in the floor pan.
    base = [(0.34, -0.18, 0.26, 0.04), (-0.34, -0.18, 0.26, 0.04),
            (-0.30, -0.86, 0.29, 0.04), (0.30, -0.86, 0.29, 0.04)]
    verts, faces = m.slab(base)
    colors = [m.mix(SEAT_DARK, SEAT_LIT, _t(v[1], -0.86, -0.18)) for v in verts]
    _mesh("KartSeatBase", verts, faces, "SeatMat", colors=colors)

    # Back: authored lying flat in XY, then stood up by a proper rotation.
    # interceptor.py's fins needed a hand-written rotate-and-shear for this;
    # rotated() is the same operation without the chance of writing a
    # reflection by accident.
    back = [(0.32, -0.02, 0.0, 0.035), (0.26, 0.58, 0.0, 0.030),
            (-0.26, 0.58, 0.0, 0.030), (-0.32, -0.02, 0.0, 0.035)]
    verts, faces = m.slab(back)
    verts = m.rotated(verts, (1, 0, 0), 74)          # lean back 16 deg
    verts = m.translated(verts, dy=-0.84, dz=0.28)
    colors = [m.mix(SEAT_DARK, SEAT_LIT, _t(v[2], 0.28, 0.78)) for v in verts]
    _mesh("KartSeatBack", verts, faces, "SeatMat", colors=colors)


def build_kart_steering():
    # The rim radius and the wheel's centre are BOTH derived from the rider
    # station, not chosen: the goblin's hands close at rider.GRIP either side
    # of the hip, so the rim has to pass through those two points. A wheel
    # sized by eye is how you get a driver whose hands hover an inch off it.
    gl, gr = _station(rider.grips(rider.KART_HIP))
    rim_r = gl[0]
    hub = (0.0, gl[1], gl[2])

    # Column: straight from a mount on the floor pan up to the hub. Built
    # with segment() so both ENDPOINTS are stated — the version before this
    # was a cylinder rotated 34 degrees and then translated by a fudge
    # factor, and when the hub moved the column stayed behind, leaving the
    # steering wheel floating in mid-air over the middle of the kart.
    _on("steer")
    base = (0.0, hub[1] - 0.34, 0.22)
    verts, faces = m.segment(base, hub, 0.048, 0.034, 5)
    colors = [m.mix(CHROME_DARK, CHROME, _t(v[2], 0.22, hub[2])) for v in verts]
    _mesh("KartColumn", verts, faces, "TrimMat", colors=colors)

    # Wheel: a torus is the one shape here that genuinely is a torus. 8x3 is
    # 48 tris — the single most expensive detail on the kart, and worth it,
    # because a kart with no steering wheel reads as a soapbox.
    verts, faces = m.torus(rim_r, 0.034, 10, 4)
    verts = m.rotated(verts, (1, 0, 0), 34)
    verts = m.translated(verts, dy=hub[1], dz=hub[2])
    colors = [m.mix(GRIP, CHROME, _t(abs(v[0]), 0.06, rim_r)) for v in verts]
    _mesh("KartWheel", verts, faces, "TrimMat", colors=colors, smooth=True)
    _on("body")


def build_kart_pedals():
    """Two pedal blocks on the floor pan, at the rider station's footrests.

    Cheap (12 tris each) and load-bearing for the animation: without them the
    seated pose's feet stop in mid-air over a flat green floor, and the eye
    reads that as the legs being too short rather than as a kart with no
    pedals.
    """
    for i, p in enumerate(_station(rider.treads(rider.KART_HIP))):
        verts, faces = m.box(p[0], p[1], p[2], 0.20, 0.24, 0.07)
        verts = m.rotated(verts, (1, 0, 0), -18, origin=p)
        colors = [m.mix(CHROME_DARK, CHROME, _t(v[2], p[2] - 0.05, p[2] + 0.05))
                  for v in verts]
        _mesh(f"KartPedal{'R' if i == 0 else 'L'}", verts, faces,
                    "TrimMat", colors=colors)


def build_kart_engine():
    # Hung off the right-hand side, where a kart's engine actually is — the
    # asymmetry is the single detail that stops this reading as a bumper car.
    verts, faces = m.box(0.64, -0.60, 0.40, 0.28, 0.44, 0.34)
    colors = [m.mix(ENGINE_CASE, ENGINE_FIN, _t(v[2], 0.24, 0.56))
              for v in verts]
    _mesh("KartEngine", verts, faces, "EngineMat", colors=colors)

    # Exhaust: up out of the engine, back over the rear axle, mouth pointing
    # away from the driver.
    path = [(0.64, -0.66, 0.56), (0.62, -0.92, 0.72),
            (0.56, -1.16, 0.80), (0.44, -1.34, 0.74)]
    build_pipe("KartPipe", path, 0.055, hot_from=3)


def build_kart_bumpers():
    # Front and rear hoops. A kart's bumpers are tube, not bodywork, so they
    # are swept and chrome rather than slabbed and accent-coloured.
    section = [(0.045 * math.cos(2 * math.pi * i / 6),
                0.045 * math.sin(2 * math.pi * i / 6)) for i in range(6)]

    front = [(-0.46, 1.30, 0.22), (-0.30, 1.52, 0.24),
             (0.30, 1.52, 0.24), (0.46, 1.30, 0.22)]
    rear = [(-0.52, -1.10, 0.30), (-0.34, -1.34, 0.34),
            (0.34, -1.34, 0.34), (0.52, -1.10, 0.30)]
    for name, path in (("KartBumperF", front), ("KartBumperR", rear)):
        verts, faces = m.sweep(path, section)
        colors = [m.mix(CHROME_DARK, CHROME, _t(abs(v[0]), 0.50, 0.05))
                  for v in verts]
        _mesh(name, verts, faces, "TrimMat", colors=colors, smooth=True)


def kart_bones():
    """A body, a steering axle, a steering wheel and four spinning wheels.

    Wheel bones point along +X, which puts their LOCAL Y down the axle — so a
    wheel spins with a rotation about its own Y and nothing else. The front
    pair hang off `axle_f`, a bone pointing +Z at the front axle's centre, so
    one rotation about ITS local Y steers both of them together and keeps
    them parallel for free. Doing the steering on each wheel instead would
    mean two rotations that have to agree, in a local frame whose roll
    Blender chose.
    """
    fy, ry = KART_WHEELBASE_F, KART_WHEELBASE_R
    bones = [
        ("body", None, _S((0, 0, 0)), _S((0, 0, 0.5))),
        ("axle_f", "body", _S((0, fy, KART_FRONT_R)),
         _S((0, fy, KART_FRONT_R + 0.3))),
        ("steer", "body", _S((0, -0.20, 0.22)), _S((0, -0.06, 0.68))),
    ]
    for side, sg in (("r", 1.0), ("l", -1.0)):
        bones.append((f"wheel_f{side}", "axle_f",
                      _S((sg * KART_TRACK_F, fy, KART_FRONT_R)),
                      _S((sg * KART_TRACK_F + 0.3, fy, KART_FRONT_R))))
        bones.append((f"wheel_r{side}", "body",
                      _S((sg * KART_TRACK_R, ry, KART_REAR_R)),
                      _S((sg * KART_TRACK_R + 0.3, ry, KART_REAR_R))))
    return bones


def build_kart():
    _on("body")
    build_kart_pan()
    build_kart_nose()
    build_kart_pods()
    build_kart_seat()
    build_kart_steering()
    build_kart_pedals()
    build_kart_engine()
    build_kart_bumpers()
    _on("body")
    for side, sign in (("R", 1.0), ("L", -1.0)):
        low = side.lower()
        # Outboard hub only: a kart's inboard wheel faces are against the
        # chassis and never seen, so the inner hub is 20 tris of nothing.
        _on(f"wheel_f{low}")
        build_wheel(f"KartWheelF{side}", sign * KART_TRACK_F,
                    KART_WHEELBASE_F, KART_FRONT_R, 0.20,
                    hub_sides=(sign,))
        _on(f"wheel_r{low}")
        build_wheel(f"KartWheelR{side}", sign * KART_TRACK_R,
                    KART_WHEELBASE_R, KART_REAR_R, 0.30,
                    hub_sides=(sign,))
    _on("body")


# ══ MOTORCYCLE ═════════════════════════════════════════════════════════════
#
# A bike is a much harder silhouette than a kart because almost all of it is
# thin: forks, bars, spokes, pipe. Anything thin drawn at 320x240 disappears,
# so every tube here is fatter than scale — the forks are 0.055 radius on a
# 0.44 wheel, which is a chopper's proportion, not a sportbike's. That is
# deliberate: it is the proportion that survives the framebuffer.

BIKE_FRONT_R = 0.36
BIKE_REAR_R = 0.39
BIKE_FRONT_Y = 1.02
BIKE_REAR_Y = -0.92
BIKE_RAKE = 26.0         # degrees the forks lean BACK from vertical
BIKE_YOKE_Z = 0.98       # top of the forks; the bars sit just above it
# Where the fork tops land, given the axle, the rake and the fork length.
# Derived rather than eyeballed: the first pass put the yoke at a y that
# looked right in the numbers and the forks arrived half a unit in front of
# it, spearing out into open air past the handlebars.
BIKE_YOKE_Y = (BIKE_FRONT_Y
               - (BIKE_YOKE_Z - BIKE_FRONT_R) * math.tan(math.radians(BIKE_RAKE)))


def build_bike_frame():
    # Backbone: steering head down to the swingarm pivot, in one sweep. The
    # section is taller than wide so the frame reads as a spine from the side
    # (where it is visible) and nearly vanishes from the front (where it
    # should).
    # The frame, the engine and the swingarm all live in the same handspan of
    # the bike, and in the first render they merged into one undifferentiated
    # grey mass. Three fixes, all of them tone rather than geometry: the frame
    # is the DARKEST of the three so the lighter engine reads as sitting in
    # it, the swingarm drops to axle height so it stops overlapping the cases,
    # and the frame section is narrow so it is a spine from the side and
    # almost nothing from the front.
    path = [(0.0, BIKE_YOKE_Y + 0.08, 0.88), (0.0, 0.42, 0.80),
            (0.0, -0.16, 0.66), (0.0, -0.62, 0.52)]
    section = [(0.065, -0.09), (0.065, 0.09), (-0.065, 0.09), (-0.065, -0.09)]
    verts, faces = m.sweep(path, section)
    colors = [m.mix(m.srgb(44, 46, 58), CHROME_DARK, _t(v[2], 0.50, 0.92))
              for v in verts]
    _mesh("BikeFrame", verts, faces, "TrimMat", colors=colors)

    # Swingarm: one plate per side, from the pivot back to the rear axle.
    arm = [(0.10, -0.58, 0.50, 0.028), (0.10, -0.92, BIKE_REAR_R, 0.028),
           (0.19, -0.92, BIKE_REAR_R, 0.028), (0.19, -0.58, 0.50, 0.028)]
    for side, points in (("R", arm), ("L", m.mirror_x(arm))):
        verts, faces = m.slab(points)
        colors = [m.mix(CHROME_DARK, CHROME, _t(v[1], -0.92, -0.58))
                  for v in verts]
        _mesh(f"BikeArm{side}", verts, faces, "TrimMat", colors=colors)


# Fuel tank: a teardrop, wide and high at the front, drawn to a point where it
# meets the seat. Six-point section, three stations — the tank is the biggest
# single piece of bodywork on the bike and where the accent colour has to do
# most of its work, so it gets the vertex density.
BIKE_TANK_PROFILE = [
    (1.00, -0.35), (0.88, 0.32), (0.55, 0.82), (0.00, 1.00),
    (-0.55, 0.82), (-0.88, 0.32), (-1.00, -0.35), (0.00, -0.85),
]

# Stations run in INCREASING y — i.e. back to front, which reads backwards
# next to the shape it describes. It is not optional: loft winds from the
# section order plus the sweep direction, so a station list that walks -Y
# produces a tank that is correct in every dimension and inside out.
BIKE_TANK_STATIONS = [
    (-0.22, 0.19, 0.13, 0.88),
    (0.26, 0.26, 0.18, 0.94),
    (0.62, 0.20, 0.13, 0.92),
]


def build_bike_tank():
    verts, faces = m.loft([_section(BIKE_TANK_PROFILE, *s)
                           for s in BIKE_TANK_STATIONS])
    colors = [body_color(v, 0.74, 1.12) for v in verts]
    _mesh("BikeTank", verts, faces, "BodyMat", colors=colors, smooth=True)


def build_bike_seat():
    seat = [(0.16, -0.26, 0.88, 0.050), (-0.16, -0.26, 0.88, 0.050),
            (-0.15, -0.86, 0.80, 0.060), (0.15, -0.86, 0.80, 0.060)]
    verts, faces = m.slab(seat)
    colors = [m.mix(SEAT_DARK, SEAT_LIT, _t(v[2], 0.78, 0.94)) for v in verts]
    _mesh("BikeSeat", verts, faces, "SeatMat", colors=colors)

    # Tail: a short taper behind the seat, so the bike ends in a point rather
    # than in a cut-off slab. Increasing y again — see BIKE_TANK_STATIONS.
    stations = [(-1.16, 0.07, 0.05, 0.80), (-0.80, 0.16, 0.10, 0.84)]
    verts, faces = m.loft([_section(BIKE_TANK_PROFILE, *s) for s in stations])
    colors = [body_color(v, 0.72, 0.94) for v in verts]
    _mesh("BikeTail", verts, faces, "BodyMat", colors=colors)


def build_bike_front():
    _on("steer")
    # Forks: two legs raked forward. Built along +Z and rotated about X, so
    # the rake is one number and not a pair of hand-solved endpoints.
    #
    # Length is measured to reach the yoke and STOP. The first pass used 0.86
    # on a 0.40 wheel and the legs speared straight through the handlebars
    # and out the top — a rod that starts at the axle has to be exactly as
    # long as the distance to the yoke, so that distance is what it is
    # computed from rather than a number that looked about right.
    # Rotating about +X carries +Z toward +Y, so a POSITIVE rake is what
    # leans the fork top backwards over the wheel. The first pass used -26
    # on the strength of a comment saying the opposite, and built a bike
    # whose forks raked forwards like a shopping trolley's castor.
    fork_len = (BIKE_YOKE_Z - BIKE_FRONT_R) / math.cos(math.radians(BIKE_RAKE))
    for side, sign in (("R", 1.0), ("L", -1.0)):
        verts, faces = m.cylinder(0.055, fork_len, 6)
        verts = m.rotated(verts, (1, 0, 0), BIKE_RAKE)
        verts = m.translated(verts, dx=sign * 0.17, dy=BIKE_FRONT_Y,
                             dz=BIKE_FRONT_R)
        colors = [m.mix(CHROME, CHROME_DARK, _t(v[2], 0.4, 1.0)) for v in verts]
        _mesh(f"BikeFork{side}", verts, faces, "TrimMat", colors=colors)

    # Steering head, bridging the fork tops.
    verts, faces = m.box(0.0, BIKE_YOKE_Y, BIKE_YOKE_Z, 0.46, 0.16, 0.13)
    colors = [m.mix(CHROME_DARK, CHROME, _t(v[2], 0.95, 1.09)) for v in verts]
    _mesh("BikeYoke", verts, faces, "TrimMat", colors=colors)

    # Handlebars: ape hangers. The path RISES from the yoke and then sweeps
    # back and out to end exactly on the rider station's grip points — which
    # is what makes one riding animation fit this and the kart both, and is
    # also why this is a chopper and not the sportbike it started as. See
    # rider.py for the full reasoning; the short version is that no other
    # motorcycle ergonomic puts the grips where a kart's wheel is.
    gl, gr = _station(rider.grips(rider.BIKE_HIP))
    bar_top = BIKE_YOKE_Z + 0.16
    bar = [
        gr,                                                   # right grip
        (gr[0] * 0.55, BIKE_YOKE_Y - 0.02, bar_top),          # right riser top
        (0.0, BIKE_YOKE_Y, bar_top - 0.02),                   # centre clamp
        (gl[0] * 0.55, BIKE_YOKE_Y - 0.02, bar_top),          # left riser top
        gl,                                                   # left grip
    ]
    section = [(0.038 * math.cos(2 * math.pi * i / 6),
                0.038 * math.sin(2 * math.pi * i / 6)) for i in range(6)]
    verts, faces = m.sweep(bar, section)
    # Grips are rubber, the rest is chrome; the split is by distance along Y
    # rather than by |x|, because on an ape hanger the ends come BACK toward
    # the rider and an |x| ramp would put rubber on the risers instead.
    colors = [m.mix(CHROME, GRIP, _t(v[1], BIKE_YOKE_Y - 0.05, gl[1] - 0.06))
              for v in verts]
    _mesh("BikeBars", verts, faces, "TrimMat", colors=colors, smooth=True)

    # The riser stems, so the bars do not appear to sprout from nothing.
    for sign in (1.0, -1.0):
        verts, faces = m.cylinder(0.032, bar_top - BIKE_YOKE_Z + 0.02, 6)
        verts = m.translated(verts, dx=sign * gl[0] * 0.55,
                             dy=BIKE_YOKE_Y - 0.02, dz=BIKE_YOKE_Z - 0.02)
        colors = [CHROME_DARK] * len(verts)
        _mesh(f"BikeRiser{'R' if sign > 0 else 'L'}", verts, faces,
                    "TrimMat", colors=colors)

    # Headlight: a short cylinder facing +Y. cylinder() builds along +Z, so
    # this is a -90 rotation about X.
    #
    # The lens is the FRONT CAP only, and it is coloured flat rather than
    # ramped — the same lesson build_wheel's hub records. A ramp along y
    # across a 6-sided drum lights the barrel as brightly as the lens and the
    # whole thing comes out looking like a beige box, which is exactly what
    # the first pass rendered.
    verts, faces = m.cylinder(0.125, 0.14, 8, cz=0.0)
    verts = m.rotated(verts, (1, 0, 0), -90)
    verts = m.translated(verts, dy=BIKE_YOKE_Y + 0.06, dz=BIKE_YOKE_Z - 0.06)
    lens_y = max(v[1] for v in verts)
    colors = [LAMP if v[1] > lens_y - 1e-4 else LAMP_RIM for v in verts]
    _mesh("BikeLamp", verts, faces, "LampMat", colors=colors)
    _on("body")


def build_bike_engine():
    verts, faces = m.box(0.0, -0.06, 0.48, 0.34, 0.44, 0.36)
    colors = [m.mix(ENGINE_CASE, ENGINE_FIN, _t(v[2], 0.32, 0.66))
              for v in verts]
    _mesh("BikeEngine", verts, faces, "EngineMat", colors=colors)

    # Cylinder jug, canted forward out of the cases. Boxy on purpose: fins are
    # what a jug looks like, and stacked boxes would cost more than the whole
    # rest of the bike.
    verts, faces = m.box(0.0, 0.0, 0.0, 0.32, 0.24, 0.28)
    verts = m.rotated(verts, (1, 0, 0), 22)
    verts = m.translated(verts, dy=0.24, dz=0.72)
    colors = [m.mix(ENGINE_FIN, ENGINE_CASE, _t(v[1], 0.06, 0.34))
              for v in verts]
    _mesh("BikeJug", verts, faces, "EngineMat", colors=colors)

    # Exhaust: down out of the jug, back along the right side, upswept mouth.
    path = [(0.14, 0.12, 0.56), (0.20, -0.30, 0.40),
            (0.24, -0.78, 0.42), (0.26, -1.20, 0.54)]
    build_pipe("BikePipe", path, 0.062, hot_from=3)


def build_bike_pegs():
    """Forward controls: a peg and its mount arm at each footrest.

    The other half of the chopper trade rider.py describes. Mid-mount pegs
    would sit under the rider's hips, which is where a bike normally puts
    them and nowhere near where a kart puts its pedals; forward controls put
    the feet out in front, which is exactly the kart posture. The mount arm
    is what stops each peg reading as a floating stick — it ties the peg back
    to the frame it hangs off.
    """
    for i, p in enumerate(_station(rider.treads(rider.BIKE_HIP))):
        side = "R" if i == 0 else "L"
        sign = 1.0 if i == 0 else -1.0

        # Peg: a short cylinder on the X axis, like the wheels.
        verts, faces = m.cylinder(0.045, 0.16, 6, cz=0.0)
        verts = m.rotated(verts, (0, 1, 0), 90 * sign)
        verts = m.translated(verts, dx=p[0], dy=p[1], dz=p[2])
        _mesh(f"BikePeg{side}", verts, faces, "TrimMat",
                    colors=[GRIP] * len(verts))

        # Mount arm back to the frame, as a plate in the XY plane laid flat.
        # CCW in XY — the right-hand list is the authored one and the left is
        # mirror_x'd off it, so getting this backwards fails on ONE side only,
        # which is precisely what test_vehicles.py caught here.
        # Authored on the RIGHT with positive x throughout, then mirrored.
        # Building it from `sign * x` AND mirroring is a double negation: the
        # left arm comes out on the right-hand side, inside out. That is the
        # failure test_vehicles.py reported on exactly one of the two.
        px = abs(p[0])
        arm = [(px, p[1] - 0.10, p[2] + 0.02, 0.026),
               (px, p[1] + 0.02, p[2] + 0.02, 0.026),
               (0.10, p[1] + 0.02, p[2] + 0.02, 0.026),
               (0.10, p[1] - 0.34, p[2] + 0.02, 0.026)]
        if sign < 0:
            arm = m.mirror_x(arm)
        verts, faces = m.slab(arm)
        _mesh(f"BikePegArm{side}", verts, faces, "TrimMat",
                    colors=[CHROME_DARK] * len(verts))


def bike_bones():
    """A body, the fork assembly, and two wheels.

    `steer` runs DOWN THE FORK — from the yoke to the front axle — rather
    than standing vertically, because a motorcycle's steering axis is the
    fork's, raked back by BIKE_RAKE. A vertical steer bone would swing the
    front wheel round a different axis than the one the forks visibly pivot
    on, and the tyre would scrub sideways as it turned.
    """
    return [
        ("body", None, _S((0, 0, 0)), _S((0, 0, 0.5))),
        ("steer", "body", _S((0, BIKE_YOKE_Y, BIKE_YOKE_Z)),
         _S((0, BIKE_FRONT_Y, BIKE_FRONT_R))),
        ("wheel_f", "steer", _S((0, BIKE_FRONT_Y, BIKE_FRONT_R)),
         _S((0.3, BIKE_FRONT_Y, BIKE_FRONT_R))),
        ("wheel_r", "body", _S((0, BIKE_REAR_Y, BIKE_REAR_R)),
         _S((0.3, BIKE_REAR_Y, BIKE_REAR_R))),
    ]


def build_bike():
    _on("body")
    build_bike_frame()
    build_bike_tank()
    build_bike_seat()
    build_bike_front()
    build_bike_engine()
    build_bike_pegs()
    # Fenders hug their wheels: the front one is short and sits ahead of the
    # yoke, the rear wraps the top and trailing edge under the tail. The
    # front one turns with the forks, so it rides `steer` — a fender that
    # stayed put while its wheel turned under it would be the giveaway.
    _on("steer")
    build_fender("BikeFenderF", 0.0, BIKE_FRONT_Y, BIKE_FRONT_R, 0.26,
                 22, 118, steps=6)
    _on("body")
    build_fender("BikeFenderR", 0.0, BIKE_REAR_Y, BIKE_REAR_R, 0.30,
                 60, 186)
    _on("wheel_f")
    build_wheel("BikeWheelF", 0.0, BIKE_FRONT_Y, BIKE_FRONT_R, 0.22)
    _on("wheel_r")
    build_wheel("BikeWheelR", 0.0, BIKE_REAR_Y, BIKE_REAR_R, 0.28)
    _on("body")


# name -> (builder, tri ceiling). Per-model ceilings; see "Tri budget".

# ── vehicle animations ─────────────────────────────────────────────────────
# Action names match goblin.py's riding set EXACTLY, so the game drives both
# with one state: play "RideLeft" on the rider and "RideLeft" on his vehicle
# and they corner together. Anything the two disagree about — a name, a
# length — shows up as a rider leaning into a turn his kart is not taking.
#
# ── Why the wheel spin is keyed in 45-degree steps ─────────────────────────
# A single key from 0 to 360 is the obvious way to write a revolution and it
# does not survive the pipeline. Blender stores these as Euler angles, the
# glTF exporter converts rotation to QUATERNIONS, and a quaternion has no way
# to express "the long way round": 0 and 360 are the same orientation, so the
# exported curve is a wheel that never moves. 45-degree steps keep every
# interpolation unambiguously short-way and the direction survives.
#
# Spin is also the one thing here that must NOT be eased. A wheel at constant
# road speed turns at constant rate; easing it would read as the vehicle
# surging between frames.
WHEELS_KART = ("wheel_fr", "wheel_fl", "wheel_rr", "wheel_rl")
WHEELS_BIKE = ("wheel_f", "wheel_r")


def _spin(length, turns, phase=0.0):
    """Constant-rate wheel rotation, keyed every 45 degrees."""
    total = 360.0 * turns
    steps = max(1, int(round(abs(total) / 45.0)))
    return [(int(round(length * i / steps)),
             {'rot': (0.0, phase + total * i / steps, 0.0)})
            for i in range(steps + 1)]


def _hold(length, rot, frames=(0,)):
    return [(f if f >= 0 else length, {'rot': rot}) for f in frames]


def vehicle_actions(armature, wheels):
    """The five riding actions, on the vehicle side."""

    def spin_all(length, turns):
        ch = {}
        for i, w in enumerate(wheels):
            # A small per-wheel phase offset so four wheels never line their
            # hub spokes up into one strobing pattern.
            ch[w] = _spin(length, turns, phase=i * 11.0)
        return ch

    # Ride — parked with the engine running. No wheel motion at all; the whole
    # read is a fast, small, irregular shake, which is what an idling
    # two-stroke does to a chassis nobody is sitting still on.
    ch = {"body": [(0,  {'rot': (0, 0, 0), 'loc': (0, 0, 0)}),
                   (3,  {'rot': (0.5, 0, 0.4), 'loc': (0, 0, 0.004)}),
                   (7,  {'rot': (-0.4, 0, -0.3), 'loc': (0, 0, -0.003)}),
                   (11, {'rot': (0.3, 0, 0.5), 'loc': (0, 0, 0.004)}),
                   (15, {'rot': (-0.5, 0, -0.2), 'loc': (0, 0, -0.002)}),
                   (20, {'rot': (0, 0, 0), 'loc': (0, 0, 0)})]}
    for w in wheels:
        ch[w] = [(0, {'rot': (0, 0, 0)}), (20, {'rot': (0, 0, 0)})]
    ch["steer"] = [(0, {'rot': (0, 0, 0)}), (20, {'rot': (0, 0.6, 0)})]
    m.make_action(armature, "Ride", ch, length=20)

    # RideDrive — rolling. Two revolutions across 40 frames, the body pitching
    # and heaving over road that is not there, and the steering making the
    # small constant corrections a real driver makes on a straight.
    ch = spin_all(40, 2.0)
    ch["body"] = [(0,  {'rot': (0, 0, 0), 'loc': (0, 0, 0)}),
                  (7,  {'rot': (1.6, 0, -0.8), 'loc': (0, 0, 0.02)}),
                  (14, {'rot': (-1.1, 0, 0.5), 'loc': (0, 0, -0.015)}),
                  (22, {'rot': (1.9, 0, 0.9), 'loc': (0, 0, 0.025)}),
                  (30, {'rot': (-0.8, 0, -0.6), 'loc': (0, 0, -0.012)}),
                  (40, {'rot': (0, 0, 0), 'loc': (0, 0, 0)})]
    ch["steer"] = [(0, {'rot': (0, 0, 0)}), (12, {'rot': (0, -4, 0)}),
                   (26, {'rot': (0, 5, 0)}), (40, {'rot': (0, 0, 0)})]
    m.make_action(armature, "RideDrive", ch, length=40)

    # RideLeft / RideRight — steer over, roll the chassis into it, and keep
    # rolling. The body ROLLS AWAY from the turn (outside suspension
    # compresses) while the rider leans into it; those opposite signs are
    # what makes a cornering pair read as weight transfer rather than as two
    # objects tilting together.
    for name, sign in (("RideLeft", 1.0), ("RideRight", -1.0)):
        ch = spin_all(30, 1.5)
        ch["body"] = [(0,  {'rot': (0, 0, 0)}),
                      (4,  {'rot': (0.4, 0, 1.2 * sign)}),
                      (12, {'rot': (-0.6, 0, -6.5 * sign)}),
                      (18, {'rot': (-0.3, 0, -5.4 * sign)}),
                      (30, {'rot': (-0.3, 0, -5.4 * sign)})]
        ch["steer"] = [(0, {'rot': (0, 0, 0)}),
                       (4, {'rot': (0, -3 * sign, 0)}),
                       (12, {'rot': (0, 26 * sign, 0)}),
                       (18, {'rot': (0, 22 * sign, 0)}),
                       (30, {'rot': (0, 22 * sign, 0)})]
        m.make_action(armature, name, ch, length=30)

    # RideBoost — squat on the rear, nose lifts, wheels go to three and a half
    # turns in the same time RideDrive takes two.
    ch = spin_all(46, 3.5)
    ch["body"] = [(0,  {'rot': (0, 0, 0), 'loc': (0, 0, 0)}),
                  (4,  {'rot': (2.0, 0, 0), 'loc': (0, 0, -0.02)}),
                  (11, {'rot': (-4.5, 0, 0), 'loc': (0, 0, 0.03)}),
                  (22, {'rot': (-2.4, 0, 0.6), 'loc': (0, 0, 0.015)}),
                  (34, {'rot': (-1.0, 0, -0.4), 'loc': (0, 0, 0.006)}),
                  (46, {'rot': (0, 0, 0), 'loc': (0, 0, 0)})]
    ch["steer"] = [(0, {'rot': (0, 0, 0)}), (46, {'rot': (0, 0, 0)})]
    m.make_action(armature, "RideBoost", ch, length=46)

    # RideHit — the chassis takes the blow and rebounds, and the wheels lose
    # then regain their rate. Spin is deliberately NOT constant here: a
    # snatched wheel is most of what makes an impact read.
    ch = {}
    for i, w in enumerate(wheels):
        ch[w] = [(0, {'rot': (0, i * 11.0, 0)}),
                 (5, {'rot': (0, i * 11.0 + 40, 0)}),
                 (16, {'rot': (0, i * 11.0 + 70, 0)}),
                 (30, {'rot': (0, i * 11.0 + 190, 0)}),
                 (52, {'rot': (0, i * 11.0 + 430, 0)})]
    ch["body"] = [(0,  {'rot': (0, 0, 0), 'loc': (0, 0, 0)}),
                  (4,  {'rot': (5.5, 0, -7.0), 'loc': (0, 0, 0.05)}),
                  (14, {'rot': (-4.0, 0, 6.0), 'loc': (0, 0, -0.03)}),
                  (25, {'rot': (2.4, 0, -3.4), 'loc': (0, 0, 0.02)}),
                  (36, {'rot': (-1.1, 0, 1.6), 'loc': (0, 0, -0.008)}),
                  (52, {'rot': (0, 0, 0), 'loc': (0, 0, 0)})]
    ch["steer"] = [(0, {'rot': (0, 0, 0)}), (5, {'rot': (0, -19, 0)}),
                   (16, {'rot': (0, 14, 0)}), (28, {'rot': (0, -7, 0)}),
                   (52, {'rot': (0, 0, 0)})]
    m.make_action(armature, "RideHit", ch, length=52)


# name -> (builder, bone table, tri ceiling).
MODELS = {
    "gokart": (build_kart, kart_bones, 900),
    "bike": (build_bike, bike_bones, 980),
}


def main():
    name = m.arg("--model", "gokart")
    if name not in MODELS:
        raise SystemExit(f"vehicles.py: no model '{name}' "
                         f"(have: {', '.join(sorted(MODELS))})")

    accent = m.arg("--accent", "")
    if accent:
        set_accent(accent)

    builder, bone_fn, max_tris = MODELS[name]
    m.reset_scene()
    PARTS.clear()
    armature = m.make_armature("VehicleRig", bone_fn())
    builder()
    m.make_skinned_mesh(name.capitalize(), PARTS, armature, "VehicleMat")
    vehicle_actions(armature, WHEELS_KART if name == "gokart" else WHEELS_BIKE)
    print(f"  [WELD] {WELD['verts_in']:>4} -> {WELD['verts_out']:>4} verts, "
          f"{WELD['tris_in']:>4} -> {WELD['tris_out']:>4} tris, "
          f"{WELD['dropped']} degenerate faces dropped")
    m.report(max_tris=max_tris)
    m.export_gltf(m.arg("--out"), animated=True)


main()
