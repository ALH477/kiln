# SPDX-License-Identifier: MPL-2.0
"""goblin.py — a goofy goblin, rigged and animated.

    blender --background --factory-startup -noaudio \
        --python tools/blender/goblin.py -- --model goblin --out build/goblin

Nothing else in this repo exercises the skinning and animation half of Tiny3D
(t3dskeleton.h, t3danim.h). This does, and it is also the only character-shaped
thing here, so it doubles as the test for whether a rigged model survives the
whole pipeline.

── Rigid segments, not smooth weights ─────────────────────────────────────
The importer allows exactly ONE bone per vertex, and at most three bones per
triangle (Tiny3D README). Blender's "with automatic weights" parenting produces
neither — it spreads each vertex across several bones, and the importer keeps
whichever it saw first, so the mesh tears at every joint in a way that looks
like a corrupt export rather than a rigging mistake.

So every piece of this goblin is a separate rigid chunk welded to exactly one
bone. That constraint is the reason he is built out of blocks, and the
articulated-wooden-puppet look it forces is leaned into rather than fought:
oversized head, hands and feet, tiny torso, everything visibly a separate part.

── Proportions ────────────────────────────────────────────────────────────
He is ~2.3 Blender units tall and stands on Z=0, so at the default
--base-scale=64 he is ~150 Tiny3D units — a bit over twice the size of
examples/engine's hand-built cube, and comfortably inside the int16 position
range (±512 units at that scale).
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import m64lib as m  # noqa: E402

# ── palette ────────────────────────────────────────────────────────────────
SKIN = m.srgb(122, 176, 74)          # sickly green
SKIN_DARK = m.srgb(84, 128, 52)
BELLY = m.srgb(168, 196, 118)        # paler underside
NOSE = m.srgb(150, 150, 70)          # a bit jaundiced, on purpose
TUNIC = m.srgb(118, 74, 52)          # grubby leather
EYE = m.srgb(248, 246, 214)
PUPIL = m.srgb(24, 20, 18)
TOOTH = m.srgb(238, 232, 200)
NAIL = m.srgb(212, 200, 160)

# ── skeleton ───────────────────────────────────────────────────────────────
# (name, parent, head, tail). The armature object itself stays at the origin
# with no transform: gltf_to_t3d throws "At least one ancestor of
# armature/skin root bone has significant transforms!" otherwise, and the fix
# is always to move the geometry, never the armature.
BONES = [
    ("root",     None,     (0.00, 0.0, 0.72), (0.00, 0.0, 0.98)),
    ("torso",    "root",   (0.00, 0.0, 0.98), (0.00, 0.0, 1.46)),
    ("head",     "torso",  (0.00, 0.0, 1.46), (0.00, 0.0, 2.10)),
    ("jaw",      "head",   (0.00, -0.10, 1.60), (0.00, -0.34, 1.50)),
    ("nose",     "head",   (0.00, -0.18, 1.80), (0.00, -0.72, 1.62)),
    ("ear_l",    "head",   (0.30, 0.0, 1.88), (0.78, 0.10, 2.20)),
    ("ear_r",    "head",   (-0.30, 0.0, 1.88), (-0.78, 0.10, 2.20)),
    ("arm_l",    "torso",  (0.36, 0.0, 1.38), (0.62, 0.0, 1.00)),
    ("hand_l",   "arm_l",  (0.62, 0.0, 1.00), (0.72, 0.0, 0.66)),
    ("arm_r",    "torso",  (-0.36, 0.0, 1.38), (-0.62, 0.0, 1.00)),
    ("hand_r",   "arm_r",  (-0.62, 0.0, 1.00), (-0.72, 0.0, 0.66)),
    ("leg_l",    "root",   (0.20, 0.0, 0.72), (0.21, 0.0, 0.24)),
    ("foot_l",   "leg_l",  (0.21, 0.0, 0.24), (0.22, -0.30, 0.06)),
    ("leg_r",    "root",   (-0.20, 0.0, 0.72), (-0.21, 0.0, 0.24)),
    ("foot_r",   "leg_r",  (-0.21, 0.0, 0.24), (-0.22, -0.30, 0.06)),
]


def _box(cx, cy, cz, sx, sy, sz, color):
    """A box as the (bone-less) (verts, faces, colors) triple parts want.

    `color` is either one srgb() tuple or a list of six, one per face. The
    six-entry form works because m64lib.box() splits all 24 corners per face,
    so nothing is averaged — which is how the eyes below get their pupils for
    free, as a darkened front face rather than extra geometry.
    """
    verts, faces = m.box(cx, cy, cz, sx, sy, sz)
    return verts, faces, m.expand_colors("box", color, verts, faces)


def _wedge(x_sign, color):
    """A floppy pointed ear: a flat triangular fin, 6 verts, 4 tris.

    Two triangles back to back rather than one, because the material culls
    back faces — a single-sided fin vanishes from half the orbit."""
    x0, x1 = 0.28 * x_sign, 0.82 * x_sign
    verts = [
        (x0, -0.10, 1.74), (x0, 0.10, 1.74), (x1, 0.06, 2.24),
        (x0, -0.10, 1.74), (x0, 0.10, 1.74), (x1, 0.06, 2.24),
    ]
    faces = [(0, 1, 2), (5, 4, 3)]
    return verts, faces, [color] * 6


def build_goblin():
    armature = m.make_armature("GoblinRig", BONES)

    parts = []

    # ── torso ──────────────────────────────────────────────────────────
    # Pot belly wider than the chest, sitting low. The tunic is a colour
    # change on the box rather than separate geometry — 12 tris saved on a
    # detail nobody will look at twice.
    parts.append(("root", *_box(0, 0, 0.84, 0.62, 0.44, 0.34, TUNIC)))
    parts.append(("torso", *_box(0, -0.03, 1.20, 0.70, 0.56, 0.52, [
        SKIN, BELLY, BELLY, SKIN_DARK, SKIN, SKIN_DARK,
    ])))

    # ── head ───────────────────────────────────────────────────────────
    # Deliberately out of proportion: about as wide as the torso and nearly
    # as tall. Most of the character reads from this one box's size.
    parts.append(("head", *_box(0, 0, 1.80, 0.78, 0.66, 0.62, [
        SKIN, SKIN, SKIN_DARK, SKIN, SKIN, SKIN,
    ])))

    # Eyes: one box each, with the front face darkened into a pupil. Faces come
    # back from m64lib.box() in the order -Z, +Z, -Y, +X, +Y, -X, so index 2 is
    # the one facing forward.
    for sx in (1, -1):
        eye_faces = [EYE, EYE, PUPIL, EYE, EYE, EYE]
        parts.append(("head", *_box(0.19 * sx, -0.30, 1.90,
                                    0.22, 0.10, 0.24, eye_faces)))

    # A big hooked nose, built as a cone laid along -Y and drooping.
    cone_v, cone_f = m.cylinder(0.17, 0.62, 5, top_radius=0.0)
    nose_v = [(x, -z - 0.14, y * 0.8 + 1.74 - z * 0.22) for x, y, z in cone_v]
    parts.append(("nose", nose_v, cone_f, [NOSE] * len(nose_v)))

    parts.append(("ear_l", *_wedge(1, SKIN)))
    parts.append(("ear_r", *_wedge(-1, SKIN)))

    # Jaw with two crooked tusks that stick out past the lip.
    parts.append(("jaw", *_box(0, -0.22, 1.50, 0.52, 0.30, 0.20, SKIN_DARK)))
    for sx, h in ((1, 0.20), (-1, 0.15)):
        parts.append(("jaw", *_box(0.14 * sx, -0.33, 1.56 + h * 0.3,
                                   0.09, 0.09, h, TOOTH)))

    # ── limbs ──────────────────────────────────────────────────────────
    # Arms hang well past the hip; hands are oversized. Both are the joke.
    for side, sx in (("l", 1), ("r", -1)):
        parts.append((f"arm_{side}",
                      *_box(0.49 * sx, 0, 1.19, 0.20, 0.20, 0.44, SKIN)))
        parts.append((f"hand_{side}",
                      *_box(0.68 * sx, 0, 0.82, 0.26, 0.22, 0.30, SKIN)))
        parts.append((f"hand_{side}",
                      *_box(0.68 * sx, -0.14, 0.70, 0.22, 0.08, 0.07, NAIL)))

        parts.append((f"leg_{side}",
                      *_box(0.20 * sx, 0, 0.48, 0.24, 0.24, 0.50, SKIN_DARK)))
        # Big flat feet, pushed forward so he looks slightly off balance.
        parts.append((f"foot_{side}",
                      *_box(0.21 * sx, -0.14, 0.10, 0.28, 0.46, 0.18, TUNIC)))

    m.make_skinned_mesh("Goblin", parts, armature, "GoblinMat")
    return armature


# ── animations ─────────────────────────────────────────────────────────────
# Keys are in degrees, and every action returns to its frame-0 pose at the last
# frame so it loops without a visible snap. The importer resamples all of this
# to a fixed 60 Hz regardless of what is keyed here (animSampleRate in
# gltf_importer/src/main.cpp), so keying densely buys nothing.

def anim_idle(armature):
    """Breathing, an ear twitch, and a nose that will not sit still. Idle is
    what he does most of the time, so it carries most of the personality."""
    m.make_action(armature, "Idle", {
        "torso": [(0, {'rot': (0, 0, 0)}), (30, {'rot': (-3, 0, 0)}),
                  (60, {'rot': (0, 0, 0)})],
        "head":  [(0, {'rot': (0, 0, 0)}), (20, {'rot': (4, 0, -3)}),
                  (44, {'rot': (-2, 0, 4)}), (60, {'rot': (0, 0, 0)})],
        "ear_l": [(0, {'rot': (0, 0, 0)}), (14, {'rot': (0, -22, 0)}),
                  (26, {'rot': (0, 6, 0)}), (60, {'rot': (0, 0, 0)})],
        "ear_r": [(0, {'rot': (0, 0, 0)}), (34, {'rot': (0, 18, 0)}),
                  (46, {'rot': (0, -5, 0)}), (60, {'rot': (0, 0, 0)})],
        "nose":  [(0, {'rot': (0, 0, 0)}), (24, {'rot': (7, 0, 0)}),
                  (60, {'rot': (0, 0, 0)})],
        "arm_l": [(0, {'rot': (0, 0, 0)}), (30, {'rot': (5, 0, 0)}),
                  (60, {'rot': (0, 0, 0)})],
        "arm_r": [(0, {'rot': (0, 0, 0)}), (30, {'rot': (5, 0, 0)}),
                  (60, {'rot': (0, 0, 0)})],
    }, length=60)


def anim_walk(armature):
    """A waddle. Legs alternate, arms counter-swing, and the root bobs at twice
    the leg frequency so the weight lands on each step."""
    m.make_action(armature, "Walk", {
        "root": [(0, {'loc': (0, 0, 0), 'rot': (0, 0, 0)}),
                 (10, {'loc': (0, 0, -0.06), 'rot': (0, 0, 6)}),
                 (20, {'loc': (0, 0, 0), 'rot': (0, 0, 0)}),
                 (30, {'loc': (0, 0, -0.06), 'rot': (0, 0, -6)}),
                 (40, {'loc': (0, 0, 0), 'rot': (0, 0, 0)})],
        "leg_l": [(0, {'rot': (28, 0, 0)}), (20, {'rot': (-28, 0, 0)}),
                  (40, {'rot': (28, 0, 0)})],
        "leg_r": [(0, {'rot': (-28, 0, 0)}), (20, {'rot': (28, 0, 0)}),
                  (40, {'rot': (-28, 0, 0)})],
        "foot_l": [(0, {'rot': (-16, 0, 0)}), (20, {'rot': (14, 0, 0)}),
                   (40, {'rot': (-16, 0, 0)})],
        "foot_r": [(0, {'rot': (14, 0, 0)}), (20, {'rot': (-16, 0, 0)}),
                   (40, {'rot': (14, 0, 0)})],
        "arm_l": [(0, {'rot': (-32, 0, 0)}), (20, {'rot': (32, 0, 0)}),
                  (40, {'rot': (-32, 0, 0)})],
        "arm_r": [(0, {'rot': (32, 0, 0)}), (20, {'rot': (-32, 0, 0)}),
                  (40, {'rot': (32, 0, 0)})],
        "head": [(0, {'rot': (0, 0, -5)}), (20, {'rot': (0, 0, 5)}),
                 (40, {'rot': (0, 0, -5)})],
        "ear_l": [(0, {'rot': (0, -14, 0)}), (20, {'rot': (0, 10, 0)}),
                  (40, {'rot': (0, -14, 0)})],
        "ear_r": [(0, {'rot': (0, 14, 0)}), (20, {'rot': (0, -10, 0)}),
                  (40, {'rot': (0, 14, 0)})],
    }, length=40)


def anim_wave(armature):
    """Right arm up, hand flapping, head tilted. Deliberately asymmetric — a
    symmetric pose would not tell you whether left and right bones got
    swapped somewhere in the export."""
    m.make_action(armature, "Wave", {
        "arm_r": [(0, {'rot': (0, 0, 0)}), (10, {'rot': (0, 0, -128)}),
                  (40, {'rot': (0, 0, -128)}), (50, {'rot': (0, 0, 0)})],
        "hand_r": [(0, {'rot': (0, 0, 0)}), (12, {'rot': (0, 0, -34)}),
                   (20, {'rot': (0, 0, 30)}), (28, {'rot': (0, 0, -34)}),
                   (36, {'rot': (0, 0, 30)}), (44, {'rot': (0, 0, 0)}),
                   (50, {'rot': (0, 0, 0)})],
        "head": [(0, {'rot': (0, 0, 0)}), (14, {'rot': (0, 0, -14)}),
                 (42, {'rot': (0, 0, -14)}), (50, {'rot': (0, 0, 0)})],
        "torso": [(0, {'rot': (0, 0, 0)}), (14, {'rot': (0, 0, -6)}),
                  (42, {'rot': (0, 0, -6)}), (50, {'rot': (0, 0, 0)})],
        "ear_r": [(0, {'rot': (0, 0, 0)}), (16, {'rot': (0, -26, 0)}),
                  (30, {'rot': (0, 12, 0)}), (50, {'rot': (0, 0, 0)})],
    }, length=50)


def anim_taunt(armature):
    """Leans in, flaps both ears, works the jaw, wiggles. The busiest action,
    and therefore the one that will show a bone-order or channel-mapping bug
    soonest."""
    m.make_action(armature, "Taunt", {
        "root": [(0, {'rot': (0, 0, 0), 'loc': (0, 0, 0)}),
                 (12, {'rot': (16, 0, 0), 'loc': (0, 0, -0.10)}),
                 (30, {'rot': (16, 0, 0), 'loc': (0, 0, -0.10)}),
                 (45, {'rot': (0, 0, 8), 'loc': (0, 0, 0)}),
                 (60, {'rot': (0, 0, 0), 'loc': (0, 0, 0)})],
        "head": [(0, {'rot': (0, 0, 0)}), (12, {'rot': (-22, 0, 0)}),
                 (36, {'rot': (-22, 0, 0)}), (60, {'rot': (0, 0, 0)})],
        "jaw": [(0, {'rot': (0, 0, 0)}), (8, {'rot': (34, 0, 0)}),
                (16, {'rot': (0, 0, 0)}), (24, {'rot': (34, 0, 0)}),
                (32, {'rot': (0, 0, 0)}), (60, {'rot': (0, 0, 0)})],
        "ear_l": [(0, {'rot': (0, 0, 0)}), (10, {'rot': (0, -38, 0)}),
                  (22, {'rot': (0, 14, 0)}), (34, {'rot': (0, -30, 0)}),
                  (60, {'rot': (0, 0, 0)})],
        "ear_r": [(0, {'rot': (0, 0, 0)}), (10, {'rot': (0, 38, 0)}),
                  (22, {'rot': (0, -14, 0)}), (34, {'rot': (0, 30, 0)}),
                  (60, {'rot': (0, 0, 0)})],
        "arm_l": [(0, {'rot': (0, 0, 0)}), (14, {'rot': (0, 0, 62)}),
                  (40, {'rot': (0, 0, 44)}), (60, {'rot': (0, 0, 0)})],
        "arm_r": [(0, {'rot': (0, 0, 0)}), (14, {'rot': (0, 0, -62)}),
                  (40, {'rot': (0, 0, -44)}), (60, {'rot': (0, 0, 0)})],
        "nose": [(0, {'rot': (0, 0, 0)}), (20, {'rot': (-16, 0, 0)}),
                 (60, {'rot': (0, 0, 0)})],
    }, length=60)


def main():
    if m.arg("--model", "goblin") != "goblin":
        raise SystemExit("goblin.py only builds 'goblin'")

    m.reset_scene()
    armature = build_goblin()

    anim_idle(armature)
    anim_walk(armature)
    anim_wave(armature)
    anim_taunt(armature)

    m.report(max_tris=280)
    m.export_gltf(m.arg("--out"), animated=True)


main()
