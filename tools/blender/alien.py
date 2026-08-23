# SPDX-License-Identifier: MIT
"""alien.py — a six-legged alien, rigged and animated.

    blender --background --factory-startup -noaudio \
        --python tools/blender/alien.py -- --model alien --out build/alien

A new actor for examples/cinematic-demo. Taller and leaner than the goblin,
six legs arranged around a vertical torso, deep purple skin with a sickly
green belly, a single red eye. Two bones (root, head) so the head can bob
forward as the alien approaches — the only animation, on purpose: it's a
background creature, not a character.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import kilnlib as m  # noqa: E402

# ── palette ────────────────────────────────────────────────────────────────
SKIN = m.srgb(106, 56, 132)           # deep purple
SKIN_DARK = m.srgb(72, 36, 92)        # shadow purple
BELLY = m.srgb(120, 162, 88)          # sickly green
EYE_GLOW = m.srgb(232, 64, 56)        # glowing red
SPINE = m.srgb(58, 28, 72)            # very dark purple for back hump

# ── skeleton ───────────────────────────────────────────────────────────────
# Two bones, both at the top of the torso so the head bobs without disturbing
# the spine. The single root bone holds the whole body rigid; the head is the
# only thing that animates. Total ~70 triangles.
BONES = [
    ("root", None,
        (0.00, 0.00, 1.20), (0.00, 0.00, 2.20)),
    ("head", "root",
        (0.00, 0.00, 2.40), (0.00, -0.30, 2.70)),
]


def _box(cx, cy, cz, sx, sy, sz, color):
    """A box as the (verts, faces, colors) triple parts want."""
    verts, faces = m.box(cx, cy, cz, sx, sy, sz)
    return verts, faces, m.expand_colors("box", color, verts, faces)


def build_alien():
    armature = m.make_armature("AlienRig", BONES)
    parts = []

    # ── torso: 3 stacked boxes ─────────────────────────────────────────
    # Lower torso, belly, upper torso — slightly different widths so the
    # silhouette has the look of an insectoid insect's three body segments.
    # The middle box carries the BELLY green on its front face.
    parts.append(("root", *_box(0, 0, 0.40, 0.50, 0.42, 0.60, [
        SKIN_DARK, SKIN, SKIN, SKIN_DARK, SKIN_DARK, SKIN_DARK,
    ])))
    parts.append(("root", *_box(0, -0.04, 1.00, 0.58, 0.48, 0.42, [
        SKIN, BELLY, BELLY, SKIN, SKIN_DARK, SKIN,
    ])))
    parts.append(("root", *_box(0, 0, 1.50, 0.52, 0.44, 0.50, [
        SKIN_DARK, SKIN, SKIN, SKIN_DARK, SKIN, SKIN_DARK,
    ])))

    # A hump along the spine — gives the silhouette the "alien-not-human"
    # silhouette without a single extra vertex.
    parts.append(("root", *_box(0, 0.14, 1.20, 0.20, 0.28, 0.42, SPINE)))

    # ── head: oval-ish dome with one glowing eye ───────────────────────
    parts.append(("head", *_box(0, 0.10, 2.60, 0.46, 0.36, 0.42, [
        SKIN, SKIN, SKIN, SKIN, SKIN_DARK, SKIN,
    ])))
    # The eye is a darkened box face — box() face 2 faces +Y in our local
    # frame, but the head is tilted; we just put the glow on whichever face
    # ends up frontmost after the head bone rotation.
    parts.append(("head", *_box(0.18, -0.02, 2.62, 0.10, 0.06, 0.12, [
        SKIN, SKIN, SKIN, SKIN, SKIN, EYE_GLOW,
    ])))

    # Two small mandibles below the head. They belong to the head bone so
    # they bob along with it.
    for sx in (1, -1):
        parts.append(("head", *_box(0.12 * sx, -0.18, 2.50,
                                    0.06, 0.18, 0.10, SKIN_DARK)))

    # ── six legs ───────────────────────────────────────────────────────
    # Three pairs, two front (skewed forward), two middle, two back
    # (skewed backward). Each leg is two segments welded to the root bone.
    # 6 legs × 2 segments = 12 boxes; ~36 tris out of ~70.
    for side, sx in (("l", 1), ("r", -1)):
        for label, x_off, z_off, ang in (
            ("f", 0.26 * sx, 0.50,  -22 if sx > 0 else  22),
            ("m", 0.30 * sx, 1.00,   0),
            ("b", 0.26 * sx, 1.50,   22 if sx > 0 else -22),
        ):
            # Upper leg segment
            parts.append(("root", *_box(x_off, -0.02, z_off,
                                        0.10, 0.10, 0.34, SKIN_DARK)))
            # Lower leg segment (foot)
            parts.append(("root", *_box(x_off + 0.04 * sx, -0.16,
                                        z_off + 0.20, 0.10, 0.18, 0.10,
                                        SKIN_DARK)))

    m.make_skinned_mesh("Alien", parts, armature, "AlienMat")
    return armature


def anim_approach(armature):
    """Head bobs forward, root sways slightly — a creature closing the
    distance, not a static prop. Loops cleanly because every key returns
    to the bind-pose value at frame 30.
    """
    m.make_action(armature, "Approach", {
        "root": [(0,  {'rot': (0, 0, 0),  'loc': (0, 0, 0)}),
                 (7,  {'rot': (0, 0, 4),  'loc': (0, 0, -0.04)}),
                 (15, {'rot': (0, 0, -4), 'loc': (0, 0,  0.02)}),
                 (22, {'rot': (0, 0, 4),  'loc': (0, 0, -0.04)}),
                 (30, {'rot': (0, 0, 0),  'loc': (0, 0, 0)})],
        "head": [(0,  {'rot': (0, 0, 0)}),
                 (5,  {'rot': (-12, 0, 0)}),
                 (12, {'rot': (-22, 0, 4)}),
                 (20, {'rot': (-12, 0, -4)}),
                 (30, {'rot': (0, 0, 0)})],
    }, length=30)


def main():
    if m.arg("--model", "alien") != "alien":
        raise SystemExit("alien.py only builds 'alien'")

    m.reset_scene()
    armature = build_alien()

    anim_approach(armature)

    m.report(max_tris=280)
    m.export_gltf(m.arg("--out"), animated=True)


main()
