# SPDX-License-Identifier: MIT
"""lab_arms.py — a pair of robotic arms for the MRI bay. Rigged and animated.

    blender --background --factory-startup -noaudio \
        --python tools/blender/lab_arms.py -- --model lab_arms --out build/lab_arms

Set dressing for PetaByte Madness' lab: two wall-mounted service arms
flanking the MRI-like scanner, each two bones (shoulder + elbow), one `idle`
loop clip — a slow servo hunt, not a gesture. Same rigid-bone-per-vertex
shape droid.py uses for its own two-bone arms, scaled up and simplified to
just the arms (no chassis/head/legs — the mount itself is the "body" here).

Authored at its own local origin, business end (the claw) generally toward
-Y, so pm_lab.c/pm_demo.c can place the whole pair with one KilnTransform
next to the MRI bay, the same way draw_at places the centaur — see
PetaByte Madness's tools/dank_lab_gen.py for where "next to" actually is.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import kilnlib as m  # noqa: E402

# ── palette ──────────────────────────────────────────────────────────────────
CHASSIS = m.srgb(70, 76, 84)          # gunmetal, matches droid.py's family
CHASSIS_DARK = m.srgb(44, 48, 54)
JOINT = m.srgb(210, 168, 40)          # hazard-yellow joint band
CLAW = m.srgb(56, 60, 66)

# ── skeleton ───────────────────────────────────────────────────────────────
# Mounted high on the wall, reaching down and forward toward table height —
# a surgical-gantry silhouette, not a human one. "R"/"L" mirror across X;
# the claw ends up near (±0.22, 0.10, 0.30), which is the point a caller
# should measure from when deciding how far out from the MRI bay to place
# the whole pair.
MOUNT_R = (0.22, 0.00, 1.00)
ELBOW_R = (0.22, 0.28, 0.62)
TIP_R = (0.22, 0.10, 0.30)
MOUNT_L = (-MOUNT_R[0], MOUNT_R[1], MOUNT_R[2])
ELBOW_L = (-ELBOW_R[0], ELBOW_R[1], ELBOW_R[2])
TIP_L = (-TIP_R[0], TIP_R[1], TIP_R[2])

BONES = [
    ("shoulder_R", None, MOUNT_R, ELBOW_R),
    ("elbow_R", "shoulder_R", ELBOW_R, TIP_R),
    ("shoulder_L", None, MOUNT_L, ELBOW_L),
    ("elbow_L", "shoulder_L", ELBOW_L, TIP_L),
]


def _box(cx, cy, cz, sx, sy, sz, color):
    verts, faces = m.box(cx, cy, cz, sx, sy, sz)
    return verts, faces, m.expand_colors("box", color, verts, faces)


def _limb(bone, a, b, r_a, r_b, color):
    verts, faces = m.segment(a, b, r_a, r_b, segments=6)
    return bone, verts, faces, m.expand_colors("limb", color, verts, faces)


def build_arms():
    armature = m.make_armature("LabArmsRig", BONES)
    parts = []

    for side, mount, elbow, tip in (
        ("R", MOUNT_R, ELBOW_R, TIP_R),
        ("L", MOUNT_L, ELBOW_L, TIP_L),
    ):
        shoulder = f"shoulder_{side}"
        elbow_bone = f"elbow_{side}"

        # Wall mount plate, bolted where the upper arm starts.
        parts.append((shoulder, *_box(*mount, 0.14, 0.14, 0.08, CHASSIS_DARK)))
        # Upper arm: mount -> elbow.
        parts.append(_limb(shoulder, mount, elbow, 0.05, 0.035, CHASSIS))
        # Joint collar at the elbow, on the SAME bone as the upper arm — it
        # visually caps the upper segment, so it should swing with it, not
        # with the forearm below.
        parts.append((shoulder, *_box(*elbow, 0.09, 0.09, 0.05, JOINT)))
        # Forearm: elbow -> tip, and the claw block, both on elbow's own bone.
        parts.append(_limb(elbow_bone, elbow, tip, 0.035, 0.02, CHASSIS))
        parts.append((elbow_bone, *_box(*tip, 0.10, 0.16, 0.06, CLAW)))

    m.make_skinned_mesh("LabArms", parts, armature, "LabArmsMat")
    return armature


def anim_idle(armature):
    """A slow servo hunt, left and right out of phase so the pair never
    holds a mirrored pose — the tell of a pre-baked loop rather than two
    independent machines."""
    m.make_action(armature, "idle", {
        "shoulder_R": [(0, {'rot': (-3, 0, 0)}),
                       (60, {'rot': (2, 0, 0)}),
                       (120, {'rot': (-3, 0, 0)})],
        "elbow_R": [(0, {'rot': (4, 0, 0)}),
                    (60, {'rot': (-2, 0, 0)}),
                    (120, {'rot': (4, 0, 0)})],
        "shoulder_L": [(0, {'rot': (2, 0, 0)}),
                       (60, {'rot': (-3, 0, 0)}),
                       (120, {'rot': (2, 0, 0)})],
        "elbow_L": [(0, {'rot': (-2, 0, 0)}),
                    (60, {'rot': (4, 0, 0)}),
                    (120, {'rot': (-2, 0, 0)})],
    }, length=120)


def main():
    if m.arg("--model", "lab_arms") != "lab_arms":
        raise SystemExit("lab_arms.py only builds 'lab_arms'")

    m.reset_scene()
    armature = build_arms()
    anim_idle(armature)

    m.report(max_tris=200)
    m.export_gltf(m.arg("--out"), animated=True)


main()
