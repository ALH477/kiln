# SPDX-License-Identifier: MPL-2.0
"""blender_test_rider.py — does the goblin's Ride pose actually reach the controls?

    blender --background --factory-startup -noaudio \
        --python tools/blender/blender_test_rider.py

Unlike test_prims.py and test_vehicles.py this one DOES need Blender, and
that is not an oversight: the thing being checked is where a posed bone ends
up, and posing a bone is Blender evaluating an armature. Reimplementing that
on the host to keep the test bpy-free would mean reimplementing the thing
under test.

── Why the name is blender_test_rider and not test_rider ──────────────────
It was `test_rider.py`, which put it in the same glob as the five bpy-free
tests — and it sat there failing with `ModuleNotFoundError: No module named
'bpy'` under a bare python3, unnoticed, because nothing ran the glob. The two
kinds of test are now distinguishable by name:

    test_*.py           bpy-free, runs under a bare python3, GATED by
                        nix/checks/blender-tests.nix
    blender_test_*.py   needs a live Blender, run by hand (or from the
                        harness at the bottom of blender-tests.nix's own
                        comment), NOT gated

The prefix is what makes the gate's exclusion deliberate rather than
accidental. Do not rename it back without also teaching the check to skip it.

── What it checks ─────────────────────────────────────────────────────────
goblin.py's riding animations are authored ONCE and played on both vehicles,
which only holds while both vehicles put their controls at rider.py's shared
offsets AND the goblin's arms and legs actually reach those offsets in the
Ride pose. Three things have to agree, and two of them are in different
files, so nothing in the normal build would notice them drifting apart: the
kart converts fine with its steering wheel moved, the goblin converts fine
with a longer forearm, and the mismatch only ever shows up as hands floating
next to a wheel in a screenshot nobody takes.

So: build the goblin, put him in the Ride pose, evaluate the armature, and
measure the distance from each hand and foot bone's tip to the grip and
footrest it is supposed to be holding. Fails past rider.MAX_SLOP.

It measures against rider.py's offsets rather than against the vehicles'
geometry directly, because that is the contract — a vehicle that honours the
offsets is by construction reachable, and test_vehicles.py already proves
the vehicles are built from them.
"""

import sys
import importlib.util
from pathlib import Path

import bpy

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

import kilnlib as m  # noqa: E402
import rider  # noqa: E402


def load_goblin():
    """Run goblin.py for its scene, without the export."""
    m.export_gltf = lambda path, animated=False: None
    m.report = lambda max_tris=None: None
    sys.argv = ["blender", "--", "--model", "goblin", "--out", "/dev/null"]
    spec = importlib.util.spec_from_file_location("gob", ROOT / "goblin.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    for obj in bpy.context.scene.objects:
        if obj.type == 'ARMATURE':
            return obj, mod
    raise SystemExit("blender_test_rider: goblin.py produced no armature")


def pose_point(armature, bone_name, end):
    """World position of a posed bone's head or tail."""
    deps = bpy.context.evaluated_depsgraph_get()
    evaluated = armature.evaluated_get(deps)
    pbone = evaluated.pose.bones[bone_name]
    return evaluated.matrix_world @ (pbone.tail if end == "tail"
                                     else pbone.head)


def apply_action(armature, name, frame):
    for track in armature.animation_data.nla_tracks:
        track.mute = (track.name != name)
    bpy.context.scene.frame_set(frame)
    bpy.context.view_layer.update()


def main():
    armature, _mod = load_goblin()
    apply_action(armature, "Ride", 0)

    # The goblin is authored standing with his root bone's head at z=0.72.
    # Mounting him is a pure translation that takes that point to the
    # vehicle's hip point, so every offset below is measured in HIS space,
    # from his own root head — which makes the check independent of where
    # either vehicle happens to sit in the world.
    root_head = armature.matrix_world @ armature.pose.bones["root"].head

    checks = []
    # Hands are measured at the bone's TIP (where the grip closes); feet at
    # the bone's HEAD, which is the ankle — see rider.py on why not the toe.
    for label, bone, end, offset, sign in (
        ("hand L -> grip L", "hand_l", "tail", rider.GRIP, +1),
        ("hand R -> grip R", "hand_r", "tail", rider.GRIP, -1),
        ("ankle L -> rest L", "foot_l", "head", rider.REST, +1),
        ("ankle R -> rest R", "foot_r", "head", rider.REST, -1),
    ):
        want = (root_head[0] + sign * offset[0],
                root_head[1] + offset[1],
                root_head[2] + offset[2])
        got = pose_point(armature, bone, end)
        d = sum((a - b) ** 2 for a, b in zip(want, got)) ** 0.5
        checks.append((label, d, want, got))

    fail = 0
    print(f"── Ride pose vs rider station (slop <= {rider.MAX_SLOP}) ──")
    for label, d, want, got in checks:
        ok = d <= rider.MAX_SLOP
        fail += 0 if ok else 1
        print(f"  {label:<20} off by {d:5.3f}   "
              f"want ({want[0]:+.2f},{want[1]:+.2f},{want[2]:+.2f})  "
              f"got ({got[0]:+.2f},{got[1]:+.2f},{got[2]:+.2f})  "
              f"{'ok' if ok else 'FAIL'}")

    print("blender_test_rider: FAILED" if fail else "blender_test_rider: rider station met")
    return 1 if fail else 0


raise SystemExit(main())
