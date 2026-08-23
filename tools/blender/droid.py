# SPDX-License-Identifier: MIT
"""droid.py — a small service droid. Rigged and animated.

    blender --background --factory-startup -noaudio \
        --python tools/blender/droid.py -- --model droid --out build/droid

A new actor for examples/cinematic-demo: a boxy service droid that orbits the
Interceptor on the launchpad. 60-ish triangles, two bones driving the arms so
the droid can wave, gunmetal + cyan + copper palette. Same rigid-bone-per-
vertex constraint goblin.py documents.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import kilnlib as m  # noqa: E402

# ── palette ────────────────────────────────────────────────────────────────
CHASSIS = m.srgb(64, 72, 84)          # gunmetal
CHASSIS_DARK = m.srgb(40, 46, 56)     # darker gunmetal for shadow faces
EYE_GLOW = m.srgb(96, 240, 248)       # cyan
JOINT = m.srgb(196, 154, 92)          # copper
HAND = m.srgb(60, 68, 78)             # dark grey

# ── skeleton ───────────────────────────────────────────────────────────────
# (name, parent, head, tail). The armature object stays at the origin with no
# transform — same rule goblin.py documents.
#
# Heights are tuned so a droid is ~1.6 Blender units tall (about as wide as the
# ship is tall, visually). Stands on Z=0.
BONES = [
    ("chassis", None,
        (0.00, 0.00, 0.40), (0.00, 0.00, 0.70)),
    ("head",    "chassis",
        (0.00, 0.00, 0.70), (0.00, 0.00, 0.96)),
    ("arm_l",   "chassis",
        (0.30, 0.00, 0.66), (0.42, 0.00, 0.40)),
    ("arm_r",   "chassis",
        (-0.30, 0.00, 0.66), (-0.42, 0.00, 0.40)),
    ("leg_l",   "chassis",
        (0.16, 0.00, 0.40), (0.16, 0.00, 0.04)),
    ("leg_r",   "chassis",
        (-0.16, 0.00, 0.40), (-0.16, 0.00, 0.04)),
]


def _box(cx, cy, cz, sx, sy, sz, color):
    """A box as the (verts, faces, colors) triple parts want."""
    verts, faces = m.box(cx, cy, cz, sx, sy, sz)
    return verts, faces, m.expand_colors("box", color, verts, faces)


def build_droid():
    armature = m.make_armature("DroidRig", BONES)
    parts = []

    # ── chassis ───────────────────────────────────────────────────────
    # Main body: wide and slightly tall, with a darker bottom so it doesn't
    # look like it floats. box() returns faces in -Z/+Z/-Y/+X/+Y/-X order,
    # so the per-face colour list below puts CHASSIS on the front/back/top
    # and CHASSIS_DARK on the bottom and sides.
    parts.append(("chassis", *_box(0, 0, 0.56, 0.66, 0.46, 0.36, [
        CHASSIS, CHASSIS, CHASSIS_DARK, CHASSIS,
        CHASSIS, CHASSIS_DARK,
    ])))

    # ── head ──────────────────────────────────────────────────────────
    # The "eye" gets the cyan glow via a darkened face on a box, exactly
    # like goblin.py's pupil trick. Front face index is 2.
    parts.append(("head", *_box(0, -0.18, 0.86, 0.50, 0.36, 0.30, [
        CHASSIS_DARK, CHASSIS, EYE_GLOW, CHASSIS, CHASSIS, CHASSIS_DARK,
    ])))

    # Two copper antenna stubs, one per head bone so they bob with the head.
    for sx in (1, -1):
        parts.append(("head", *_box(0.10 * sx, -0.16, 1.08,
                                    0.04, 0.04, 0.10, JOINT)))

    # ── arms ──────────────────────────────────────────────────────────
    # Each arm is two segments hinged on the chassis side. Upper arm and
    # forearm welded to the same bone; this is rigid skinning, so they move
    # together but each arm has its own bone.
    for side, sx in (("l", 1), ("r", -1)):
        parts.append((f"arm_{side}",
                      *_box(0.36 * sx, -0.02, 0.54, 0.16, 0.16, 0.34, CHASSIS)))
        parts.append((f"arm_{side}",
                      *_box(0.42 * sx, -0.02, 0.30, 0.20, 0.16, 0.18, HAND)))

    # ── legs ──────────────────────────────────────────────────────────
    # Two stubby legs at the corners; feet angled forward for a "rolling"
    # look.
    for side, sx in (("l", 1), ("r", -1)):
        parts.append((f"leg_{side}",
                      *_box(0.16 * sx, 0, 0.20, 0.18, 0.18, 0.36, CHASSIS_DARK)))
        parts.append((f"leg_{side}",
                      *_box(0.18 * sx, -0.08, 0.04, 0.22, 0.30, 0.08, JOINT)))

    m.make_skinned_mesh("Droid", parts, armature, "DroidMat")
    return armature


def anim_wave(armature):
    """Right arm up, the left one slightly counterbalancing, head tilted.

    The wave is the only thing the droids do — they orbit while waving at
    the goblin captain as the camera sweeps over them.
    """
    m.make_action(armature, "Wave", {
        "head":  [(0, {'rot': (0, 0, 0)}),
                  (10, {'rot': (0, 0, -6)}),
                  (40, {'rot': (0, 0, -6)}),
                  (50, {'rot': (0, 0, 0)})],
        "arm_r": [(0, {'rot': (0, 0, 0)}),
                  (8,  {'rot': (0, 0, -126)}),
                  (16, {'rot': (0, 0, -100)}),
                  (24, {'rot': (0, 0, -126)}),
                  (32, {'rot': (0, 0, -100)}),
                  (40, {'rot': (0, 0, -126)}),
                  (50, {'rot': (0, 0, 0)})],
        "arm_l": [(0, {'rot': (0, 0, 0)}),
                  (8,  {'rot': (0, 0, 14)}),
                  (40, {'rot': (0, 0, 14)}),
                  (50, {'rot': (0, 0, 0)})],
    }, length=50)


def anim_idle(armature):
    """A subtle hover-bob for when the droid isn't waving.

    Used when the goblin's walk-cycle should breathe along with the world,
    not the wave — gives the cinematic a beat between camera moves.
    """
    m.make_action(armature, "Idle", {
        "chassis": [(0, {'loc': (0, 0, 0)}),
                    (15, {'loc': (0, 0.02, 0)}),
                    (30, {'loc': (0, 0, 0)}),
                    (45, {'loc': (0, 0.02, 0)}),
                    (60, {'loc': (0, 0, 0)})],
        "head": [(0, {'rot': (0, 0, 0)}),
                 (30, {'rot': (0, 0, -3)}),
                 (60, {'rot': (0, 0, 0)})],
    }, length=60)


def main():
    if m.arg("--model", "droid") != "droid":
        raise SystemExit("droid.py only builds 'droid'")

    m.reset_scene()
    armature = build_droid()

    anim_idle(armature)
    anim_wave(armature)

    m.report(max_tris=200)
    m.export_gltf(m.arg("--out"), animated=True)


main()
