# SPDX-License-Identifier: MPL-2.0
"""horner.py — Dr. Patrick Horner, rebuilt as a rigged, animated glTF.

    blender --background --factory-startup -noaudio --python horner.py \
        -- --rig PetaByte-Madness/assets/rig/horner.json \
           --out build/horner.gltf
    ... --selftest      # build, verify the pose against the rig, exit

── Why this exists ────────────────────────────────────────────────────────
Horner shipped as a static, unrigged OBJ (patrick_horner_gen.py), imported
as a plain rigid prop by pm_props.py — fine for a body that never moves, but
the intake sequence needs him to act distraught, sit at a console and climb
into the scanner, which a rigid mesh cannot do.

`PetaByte-Madness/tools/ph_rig.py` already has a complete 17-joint skeleton
for him — rigid per-vertex binding, FK posing — but it is an N64 rigid-
segment puppet (one Vtx array and display list per joint), not a skin. This
script is the same move `centaur.py` made for the machine centaur: rebuild
that rig as a real Blender armature with vertex groups, so gltf_to_t3d emits
a skin and named animation clips this engine's `m64_skel` can play.

── Everything comes from the JSON ─────────────────────────────────────────
Mesh, rig AND clips are read from one file produced by
`PetaByte-Madness/tools/ph_rig_export.py`, which imports the real generator
(`ph_rig.py`) and the real clip choreography (`ph_anim_clips.py`) and does
its own numerical self-test of the coordinate/Euler conversion. This script
does no conversion and no choreography — if a limb is in the wrong place or
a clip is wrong, the bug is upstream, not here. Same separation as
centaur.py/mc_rig_export.py, for the same reason: the maths is testable with
a bare python3, with no Blender in the loop.

── One bone per vertex, same as the centaur ───────────────────────────────
ph_rig.py's own file comment says why the rig is rigid, not smooth-skinned:
the RSP has no skinning path, so every N64 character was rigid segments
under their own matrix, and the mesh's elbow/knee bulges exist to fill the
wedge a rigid bend opens. That is exactly m64lib's make_skinned_mesh
constraint (one bone per vertex, at most three per triangle) — not a
workaround here either, just this character's native shape carried
forward from ph_rig.py to a skin.

── Every bone points +Y, and that is deliberate ───────────────────────────
Same reasoning as centaur.py: an identity rest basis (tail = head + (0,L,0),
zero roll) means pose-bone rotation is expressed directly in armature space,
which is the space ph_rig_export.py's keyframes are already in. Nothing
hand-poses this rig in Blender, so the alternative (tail = child's head)
would only cost correctness for no readability benefit anyone needs.
"""

import json
import math
import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m64lib as m  # noqa: E402

# Leaf bone length in Blender units. Cosmetic, same as centaur.py's
# BONE_LEN — rigid skinning uses the explicit vertex groups, never the
# bone's extent.
BONE_LEN = 0.08


def build_armature(rig):
    bones = []
    for b in rig["bones"]:
        head = tuple(b["pivot"])
        tail = (head[0], head[1] + BONE_LEN, head[2])
        bones.append((b["name"], b["parent"], head, tail))

    armature = m.make_armature("HornerRig", bones)

    # make_armature sets 'XYZ'; ph_rig_export.py's coordinate change gives
    # this rig 'YZX' (see that file's comment for the derivation — ph_rig.py
    # composes Rx.Ry.Rz, the opposite order from the centaur generator's
    # Rz.Ry.Rx, so the two rigs land on different Blender Euler orders even
    # though both go through the same Y-up-to-Z-up axis change).
    order = rig["euler_order"]
    for pbone in armature.pose.bones:
        pbone.rotation_mode = order

    return armature


def build_meshes(rig, armature):
    """One mesh per material, rigidly bound, vertex groups from the exporter.

    Same shape as centaur.py's build_meshes: one bone per VERTEX (not per
    part), so a triangle spanning a seam keeps each corner on its own bone.
    """
    mesh = rig["mesh"]
    verts = [tuple(v) for v in mesh["verts"]]
    colors = [tuple(c) for c in mesh["colors"]]
    uvs = [tuple(uv) for uv in mesh["uvs"]]
    vert_bone = mesh["vert_bone"]

    objects = []
    for material, tris in sorted(mesh["prims"].items()):
        used = sorted({i for tri in tris for i in tri})
        remap = {old: new for new, old in enumerate(used)}

        m.make_material(material)
        obj = m.make_mesh(
            material,
            [verts[i] for i in used],
            [tuple(remap[i] for i in tri) for tri in tris],
            material,
            colors=[colors[i] for i in used],
            uvs=[uvs[i] for i in used],
            smooth=False,
        )

        groups = {}
        for old in used:
            groups.setdefault(vert_bone[old], []).append(remap[old])
        for bone_name, indices in groups.items():
            obj.vertex_groups.new(name=bone_name).add(indices, 1.0, 'REPLACE')

        obj.parent = armature
        obj.modifiers.new(name="Armature", type='ARMATURE').object = armature
        objects.append(obj)

    return objects


def build_actions(rig, armature):
    for anim in rig["anims"]:
        channels = {}
        for bone_name, keys in anim["tracks"].items():
            channels[bone_name] = [
                (k["frame"], {"rot": tuple(k["rot"])}) for k in keys
            ]
        m.make_action(armature, anim["name"], channels, length=anim["length"])


# ── self-test ──────────────────────────────────────────────────────────────
def selftest(rig, armature, tol=1e-4):
    """Assert Blender poses this rig the way the source rig says it should —
    same check centaur.py runs, same reason: ph_rig_export.py already
    proved its coordinate/Euler change is exact arithmetic on its own model,
    but not that Blender's bone spaces and Euler order agree with it."""
    scene = bpy.context.scene
    depsgraph = bpy.context.evaluated_depsgraph_get()

    expected = {b["name"]: b["pivot"] for b in rig["bones"]}
    worst = 0.0
    worst_at = None
    checked = 0

    # Every clip lives in its own NLA track and evaluates unless muted — see
    # centaur.py's selftest for why this matters (assigning one action on
    # top of live strips poses the rig with all of them at once).
    for track in armature.animation_data.nla_tracks:
        track.mute = True

    for anim in rig["anims"]:
        track = armature.animation_data.nla_tracks.get(anim["name"])
        strip = track.strips[0]
        armature.animation_data.action = strip.action

        # A bone this action never keys has NO fcurve in it, and Blender
        # does not reset an unkeyed property when the active action
        # changes — it is left at whatever a PREVIOUS action's construction
        # (make_action's own direct pbone.rotation_euler assignments) last
        # set it to. Reset every bone before evaluating this action, or a
        # later clip's stale rotation leaks into an earlier clip's check on
        # a joint that clip never touches.
        for pbone in armature.pose.bones:
            pbone.rotation_euler = (0.0, 0.0, 0.0)

        frames = sorted({k["frame"] for keys in anim["tracks"].values()
                         for k in keys})
        for frame in frames:
            scene.frame_set(frame)
            depsgraph.update()
            for bone_name, rest in expected.items():
                pbone = armature.pose.bones[bone_name]
                got = armature.matrix_world @ pbone.matrix.translation
                want = _expected_head(rig, anim, bone_name, frame)
                d = max(abs(a - b) for a, b in zip(got, want))
                if d > worst:
                    worst, worst_at = d, (anim["name"], bone_name, frame)
                checked += 1

    armature.animation_data.action = None
    for track in armature.animation_data.nla_tracks:
        track.mute = False

    if worst > tol:
        raise SystemExit(
            "SELFTEST FAILED: %s bone '%s' frame %d is %.6f off.\n"
            "  Blender's pose does not match the source rig. Suspect the\n"
            "  Euler order (%s), the identity rest basis, or the space\n"
            "  pose-bone rotation is interpreted in."
            % (worst_at[0], worst_at[1], worst_at[2], worst, rig["euler_order"]))
    print("  selftest OK: %d bone-poses, worst delta %.3g" % (checked, worst))


def _expected_head(rig, anim, bone_name, frame):
    """Where the source rig says this bone's pivot is, at this frame — a
    direct transcription of ph_rig.py's own matrices(): rotate, then
    translate by the bone's LOCAL (parent-relative) offset, concatenated
    onto the parent. Parents precede children in `bones`, so one pass is
    enough. NOT the same shape as centaur's _expected_head — ph_rig.py
    composes "translate-then-rotate" from an already-relative offset, the
    machine centaur generator composes "rotate about an absolute pivot,
    then translate". Reusing the wrong one would agree with itself while
    both are wrong.
    """
    bones = {b["name"]: b for b in rig["bones"]}
    order = [b["name"] for b in rig["bones"]]  # parents precede children

    local = {}
    for name in order:
        b = bones[name]
        piv = b["pivot"]
        parent = b["parent"]
        ppiv = bones[parent]["pivot"] if parent else (0.0, 0.0, 0.0)
        local[name] = tuple(piv[i] - ppiv[i] for i in range(3))

    world = {}
    for name in order:
        b = bones[name]
        rx, ry, rz = _sample(anim["tracks"].get(name), frame)
        rot = _euler_matrix(rx, ry, rz, rig["euler_order"])
        off = local[name]
        if b["parent"] is None:
            world[name] = (rot, off)
        else:
            prot, porigin = world[b["parent"]]
            world[name] = (
                [[sum(prot[i][k] * rot[k][j] for k in range(3))
                  for j in range(3)] for i in range(3)],
                [porigin[i] + sum(prot[i][k] * off[k] for k in range(3))
                 for i in range(3)],
            )
    _, origin = world[bone_name]
    return origin


def _sample(keys, f):
    if not keys:
        return (0.0, 0.0, 0.0)
    flat = [(k["frame"], *k["rot"]) for k in keys]
    if f <= flat[0][0]:
        return tuple(flat[0][1:])
    if f >= flat[-1][0]:
        return tuple(flat[-1][1:])
    for a, b in zip(flat, flat[1:]):
        if a[0] <= f <= b[0]:
            t = (f - a[0]) / float(b[0] - a[0]) if b[0] != a[0] else 0.0
            return tuple(a[1 + k] + (b[1 + k] - a[1 + k]) * t for k in range(3))
    return tuple(flat[-1][1:])


def _euler_matrix(rx, ry, rz, order):
    c = [math.cos(math.radians(v)) for v in (rx, ry, rz)]
    s = [math.sin(math.radians(v)) for v in (rx, ry, rz)]
    R = {
        'X': [[1, 0, 0], [0, c[0], -s[0]], [0, s[0], c[0]]],
        'Y': [[c[1], 0, s[1]], [0, 1, 0], [-s[1], 0, c[1]]],
        'Z': [[c[2], -s[2], 0], [s[2], c[2], 0], [0, 0, 1]],
    }
    out = [[1.0 if i == j else 0.0 for j in range(3)] for i in range(3)]
    # Blender order 'o0 o1 o2' composes as R[o2]@R[o1]@R[o0] — rightmost
    # character in the mode string applied first. See ph_rig_export.py's
    # file comment for the derivation that gives this rig 'YZX'.
    for axis in reversed(order):
        a = R[axis]
        out = [[sum(out[i][k] * a[k][j] for k in range(3)) for j in range(3)]
               for i in range(3)]
    return out


def main():
    m.reset_scene()

    rig_path = m.arg("--rig")
    with open(rig_path) as f:
        rig = json.load(f)

    print("── horner: %d bones, %d anims, %d verts ──"
          % (len(rig["bones"]), len(rig["anims"]), len(rig["mesh"]["verts"])))

    armature = build_armature(rig)
    build_meshes(rig, armature)
    build_actions(rig, armature)

    selftest(rig, armature)

    rest = [a for a in sys.argv[sys.argv.index("--") + 1:]]
    if "--selftest" in rest:
        return
    m.export_gltf(m.arg("--out"), animated=True)


if __name__ == "__main__":
    main()
