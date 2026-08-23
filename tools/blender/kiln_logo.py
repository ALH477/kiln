# SPDX-License-Identifier: MIT
"""kiln_logo.py — the Kiln boot logo, as a 3D model.

    blender --background --factory-startup -noaudio --python kiln_logo.py \
        -- --model kiln_logo --out build/kiln_logo.gltf

A classical brick kiln, not a wordmark: a squat beehive furnace with a dark
arched doorway, a flame licking out of it, and a chimney venting above. The
name reads twice this way — the engine and the object both being a "kiln" —
and it gives the boot sequence something with actual depth and heat to it,
where the old N64-parody wordmark only had extruded letters.

Three objects, exported separately so the splash can move them
independently (kiln_splash.c looks two of them up by name):

  `kiln`   the furnace body — base, tapered dome, chimney. The piece that
           spins to rest, exactly as the old wordmark did.
  `flame`  a small cluster of licks at the doorway. Looked up separately so
           the splash can give it its own flicker — an independent wobble
           layered on top of the kiln's settle, not baked into the mesh.
  `plate`  a thin ember-red bar under everything, unchanged in spirit from
           the old accent plate: it fades up late and the publisher line
           sits over it in the 2D pass.

── Two colouring strategies, on purpose ─────────────────────────────────
The kiln body is coloured PER FACE, banded into courses (base / body /
neck get progressively darker terracotta) — the same trick the old
wordmark used for its extrusion sides, and the right one for masonry: brick
courses are discrete steps, not a gradient.

The flame is coloured PER VERTEX, with SHARED vertices (unlike box()'s
split corners) so the colour actually blends smoothly base-to-tip across a
face — a real gradient, produced by the RDP's own vertex-colour
interpolation (RDPQ_COMBINER_SHADE) rather than faked with more bands. Fire
does not have facets; letters and brick courses do.

── Scale ─────────────────────────────────────────────────────────────────
Authored at "1 unit tall" felt like the natural size to draw a kiln at, and
rendered as a barely-visible fleck under kiln_splash.c's existing fixed
camera — which is framed for the old wordmark's ~3.1-unit span, not a
kiln's ~1-unit footprint. SCALE (below) corrects the overall size without
touching the proportions or kiln_splash.c's camera/transform numbers; found
by rendering through the host backend and looking, not by calculation —
see nix/checks/kiln-splash.nix.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kilnlib as m  # noqa: E402

SEGMENTS = 12  # radial resolution; chunky on purpose, matches the wordmark's
               # "read at 240 lines" aesthetic rather than a smooth render

# The old wordmark spanned ~3.1 Blender units wide, which is what
# kiln_splash.c's fixed camera (cam_pos/cam_target, unchanged here) is framed
# for. A kiln authored at "1 unit tall" felt natural to draw but rendered as a
# barely-visible fleck at that framing — confirmed by rendering it through the
# host backend before touching kiln_splash.c at all. SCALE closes that gap
# without changing any of the proportions tuned below.
SCALE = 2.4

# ── Kiln body: base -> tapered dome -> neck, stacked cylinders ──────────
R_BASE = SCALE * 0.58
H_BASE = SCALE * 0.09

R_BODY_FOOT = R_BASE       # no step where the base meets the dome —
R_SHOULDER = SCALE * 0.30  # the taper is the only silhouette change there
H_BODY = SCALE * 0.56

R_NECK = SCALE * 0.16      # deliberately narrower than R_SHOULDER, so the
H_NECK = SCALE * 0.19      # body's top cap shows as a collar the chimney sits on

# Courses get darker toward the top, as if the neck sees the most soot.
BRICK_BASE = m.srgb(146, 66, 42)
BRICK_BODY = m.srgb(128, 54, 34)
BRICK_NECK = m.srgb(94, 42, 28)

# ── Doorway: a dark recess punched through the base/lower body ──────────
# kiln_splash.c's rest pose (spin=0) faces the camera along -Y, confirmed by
# rendering all four quarter-turns through the host backend and looking —
# the CAMERA-FACING side at rest is -Y, not +Y, which is easy to get backwards
# since the wordmark this replaced had no front/back to get wrong. Front-face
# of the taper at this height is at roughly Y=-0.50 (R_BODY_FOOT tapering
# toward R_SHOULDER), so the door is centred to just reach it, biased deep
# into the body (toward Y=0) so the recess reads as hollow rather than as a
# black plate stuck onto the surface.
DOOR_W, DOOR_H = SCALE * 0.22, SCALE * 0.24
DOOR_D = SCALE * 0.50
DOOR_CZ = H_BASE + SCALE * 0.18
DOOR_CY = SCALE * -0.25
DOOR_COLOR = m.srgb(16, 11, 9)

# ── Flame: a small cluster of licks at the doorway ───────────────────────
FLAME_BASE = m.srgb(255, 88, 20)
FLAME_MID = m.srgb(255, 158, 36)
FLAME_TIP = m.srgb(255, 230, 150)

# ── Plate: the accent bar under everything ───────────────────────────────
PLATE_COLOR = m.srgb(210, 64, 24)   # ember red-orange, echoing the flame
                                    # rather than the old cold crimson


def add_band(parts, radius, height, cz, top_radius, color, segments=SEGMENTS):
    """One cylinder course, coloured per face — flat courses, not a gradient.
    Caps are given the same colour as the side: both base's bottom cap and
    body's bottom cap are fully occluded by construction (each course's
    foot radius matches the course below), so their colour is never seen."""
    verts, faces = m.cylinder(radius, height, segments, cz=cz,
                              top_radius=top_radius)
    base = len(parts["verts"])
    parts["verts"].extend(verts)
    parts["faces"].extend(tuple(base + i for i in f) for f in faces)
    n_side = segments
    n_bottom = segments if radius != 0.0 else 0
    n_top = segments if top_radius != 0.0 else 0
    parts["colors"].extend([color] * (n_side + n_bottom + n_top))


def add_door(parts):
    """The recess. A plain box driven deep enough through the front surface
    that no gap shows regardless of the exact radius at that height."""
    verts, quads = m.box(0.0, DOOR_CY, DOOR_CZ, DOOR_W, DOOR_D, DOOR_H)
    base = len(parts["verts"])
    parts["verts"].extend(verts)
    parts["faces"].extend(tuple(base + i for i in q) for q in quads)
    parts["colors"].extend([DOOR_COLOR] * len(quads))


def build_kiln():
    parts = {"verts": [], "faces": [], "colors": []}
    add_band(parts, R_BASE, H_BASE, 0.0, R_BODY_FOOT, BRICK_BASE)
    add_band(parts, R_BODY_FOOT, H_BODY, H_BASE, R_SHOULDER, BRICK_BODY)
    add_band(parts, R_NECK, H_NECK, H_BASE + H_BODY, R_NECK, BRICK_NECK)
    add_door(parts)

    m.make_material("kiln")
    m.make_mesh("kiln", parts["verts"], parts["faces"], "kiln",
                colors=parts["colors"], smooth=False)


def flame_tongue(parts, cx, cy, cz, w, d, h, lean_x=0.0, lean_y=0.0):
    """One lick: a 9-vertex shared-vertex shape (base ring -> pinched waist
    ring -> tip), so colour interpolates smoothly across every face instead
    of stepping between flat-shaded facets. The waist gives the gradient a
    second stop (base -> mid -> tip), which reads more like a flame's hot
    core than a single linear blend would."""
    hw, hd = w / 2.0, d / 2.0
    wf = 0.45   # waist pinch, fraction of the base half-extents
    lx, ly = lean_x * 0.5, lean_y * 0.5
    base = len(parts["verts"])
    verts = [
        (cx - hw, cy - hd, cz), (cx + hw, cy - hd, cz),
        (cx + hw, cy + hd, cz), (cx - hw, cy + hd, cz),
        (cx - hw * wf + lx, cy - hd * wf + ly, cz + h * 0.55),
        (cx + hw * wf + lx, cy - hd * wf + ly, cz + h * 0.55),
        (cx + hw * wf + lx, cy + hd * wf + ly, cz + h * 0.55),
        (cx - hw * wf + lx, cy + hd * wf + ly, cz + h * 0.55),
        (cx + lean_x, cy + lean_y, cz + h),
    ]
    b = base
    faces = [
        (b + 0, b + 1, b + 5, b + 4), (b + 1, b + 2, b + 6, b + 5),
        (b + 2, b + 3, b + 7, b + 6), (b + 3, b + 0, b + 4, b + 7),
        (b + 4, b + 5, b + 8), (b + 5, b + 6, b + 8),
        (b + 6, b + 7, b + 8), (b + 7, b + 4, b + 8),
    ]
    colors = [FLAME_BASE] * 4 + [FLAME_MID] * 4 + [FLAME_TIP]
    parts["verts"].extend(verts)
    parts["faces"].extend(faces)
    parts["colors"].extend(colors)   # one colour per VERTEX added above, not
                                     # per face — this is what makes it a
                                     # gradient instead of a banded fake


def build_flame():
    """A small irregular cluster, staggered in width/height/lean so it does
    not read as one shape stamped four times. Positioned just proud of the
    doorway's front face (a touch further along -Y than the door reaches),
    tall enough that the tips clear the door's top edge — flames escaping
    the opening, not tucked inside the silhouette of the black recess."""
    parts = {"verts": [], "faces": [], "colors": []}
    door_front_y = DOOR_CY - DOOR_D / 2.0     # the door's front face
    door_bottom_z = DOOR_CZ - DOOR_H / 2.0    # where the licks root
    y0 = door_front_y - SCALE * 0.06
    z0 = door_bottom_z
    s = SCALE
    flame_tongue(parts, s * -0.07, y0, z0, s * 0.15, s * 0.13, s * 0.40,
                lean_x=s * -0.02, lean_y=s * -0.05)
    flame_tongue(parts, s * 0.02, y0 - s * 0.02, z0 - s * 0.01,
                s * 0.19, s * 0.15, s * 0.52, lean_x=s * 0.02, lean_y=s * -0.04)
    flame_tongue(parts, s * 0.13, y0 - s * 0.01, z0, s * 0.13, s * 0.12, s * 0.34,
                lean_x=s * 0.04, lean_y=s * -0.06)
    flame_tongue(parts, s * -0.02, y0 - s * 0.03, z0 + s * 0.03,
                s * 0.11, s * 0.10, s * 0.24, lean_x=0.00, lean_y=s * -0.07)

    m.make_material("flame")
    m.make_mesh("flame", parts["verts"], parts["faces"], "flame",
                colors=parts["colors"], smooth=True)


def build_plate():
    """A thin bar under the kiln. The splash fades it up late, with the
    publisher line drawn over it in the 2D pass — text stays 2D because a
    3D "Kiln Engine - MIT Licensed" would need a font mesh for one screen."""
    parts = {"verts": [], "faces": [], "colors": []}
    verts, quads = m.box(0.0, 0.0, SCALE * -0.14, SCALE * 1.55, SCALE * 0.30,
                        SCALE * 0.05)
    parts["verts"].extend(verts)
    parts["faces"].extend(quads)
    parts["colors"].extend([PLATE_COLOR] * len(quads))
    m.make_material("plate")
    m.make_mesh("plate", parts["verts"], parts["faces"], "plate",
                colors=parts["colors"], smooth=False)


def main():
    m.reset_scene()
    build_kiln()
    build_flame()
    build_plate()
    m.report()
    m.export_gltf(m.arg("--out"))


main()
