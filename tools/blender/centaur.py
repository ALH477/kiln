# SPDX-License-Identifier: MPL-2.0
"""centaur.py — the machine centaur, rebuilt as a rigged, animated glTF.

    blender --background --factory-startup -noaudio --python centaur.py \
        -- --rig PetaByte-Madness/assets/rig/machine_centaur.json \
           --out build/centaur.gltf
    ... --selftest      # build, verify the pose against the rig, exit

── Why this exists ────────────────────────────────────────────────────────
PetaByte Madness' centaur arrived as F3DEX2: `reference/machine_centaur.h`,
23 bones and 13 animations whose payload is `const Gfx *dl[3]`. This engine
does not consume display lists. Worse, the first attempt at bringing the
project's characters over went through their `.glb` files, and `gltf_to_t3d`
dropped every animation channel with `Channel target not found` — they are
NODE animations on an unskinned hierarchy, and the importer wants a skin.

The centaur is one bone per limb, which means one bone per vertex, which is
exactly the constraint kilnlib's make_skinned_mesh documents:

    the importer allows ONE bone per vertex and at most three bones per
    triangle (Tiny3D README)

So an armature with rigid vertex groups is not a workaround here — it is the
native shape of this character. Built that way, the animations survive.

── Everything comes from the JSON ─────────────────────────────────────────
Mesh AND rig are read from one file produced by
`PetaByte-Madness/tools/mc_rig_export.py`, which imports the original
generator and converts coordinates (with its own numerical self-test). This
script does no conversion and no geometry authoring — if a limb is in the
wrong place, the bug is in the exporter, not here. That separation is why the
exporter can be tested with a bare `python3` and no Blender.

── Every bone points +Y, and that is deliberate ───────────────────────────
Blender's canonical bone points along +Y, so a bone whose tail is
head + (0, L, 0) with zero roll has an IDENTITY rest basis. Pose-bone
`rotation_euler` and `location` are expressed in bone space, so making every
rest basis identity means both are expressed in armature space — which is the
space the source rig's keyframes are already in.

The alternative (tail = the first child's head, so the armature looks like a
skeleton) makes each bone's local space depend on where its child happens to
sit, and silently reinterprets every translation key. The hull's bob and the
recoil on `fire` would land in the wrong direction, and it would look like a
data problem rather than a rigging one. Nothing hand-animates this rig, so the
readable-armature version buys nothing and costs correctness.
"""

import json
import math
import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kilnlib as m  # noqa: E402

# Leaf/parallel bone length in Blender units. Cosmetic — rigid skinning uses
# the explicit vertex groups, never the bone's extent.
BONE_LEN = 0.08


def build_armature(rig):
    bones = []
    for b in rig["bones"]:
        head = tuple(b["pivot"])
        tail = (head[0], head[1] + BONE_LEN, head[2])
        bones.append((b["name"], b["parent"], head, tail))

    armature = m.make_armature("CentaurRig", bones)

    # make_armature sets 'XYZ'; this rig's Euler order came out of the
    # coordinate change as 'XZY' (see mc_rig_export.py). Setting it wrong
    # does not error — it silently poses every rotated bone differently.
    order = rig["euler_order"]
    for pbone in armature.pose.bones:
        pbone.rotation_mode = order

    return armature


def build_meshes(rig, armature):
    """One mesh per material, rigidly bound, vertex groups from the exporter.

    Not kilnlib's make_skinned_mesh: that takes parts as (bone, verts, faces),
    i.e. one bone per PART. This mesh binds one bone per VERTEX, so a triangle
    spanning a seam keeps each corner on its own bone. Same tail as
    make_skinned_mesh — vertex groups, parent, armature modifier — applied to
    a different grouping.
    """
    mesh = rig["mesh"]
    verts = [tuple(v) for v in mesh["verts"]]
    colors = [m.srgb(*c) for c in mesh["colors"]]
    uvs = [tuple(uv) for uv in mesh["uvs"]]
    vert_bone = mesh["vert_bone"]

    objects = []
    for material, tris in sorted(mesh["prims"].items()):
        # Keep only the vertices this material actually uses, and remap.
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
                (k["frame"], {"rot": tuple(k["rot"]), "loc": tuple(k["pos"])})
                for k in keys
            ]
        m.make_action(armature, anim["name"], channels, length=anim["length"])


# ── self-test ──────────────────────────────────────────────────────────────
def selftest(rig, armature, tol=1e-4):
    """Assert Blender poses this rig the way the source rig says it should.

    mc_rig_export.py already proved its coordinate change is exact, but that
    check is arithmetic on its own model. This one is the part it cannot see:
    that Blender's bone spaces, Euler order and pose-bone `location`
    interpretation agree with it. Translation is the risk — `location` is in
    bone space, and it is only the same as armature space because every bone
    was built with an identity rest basis. If that assumption is ever wrong,
    the bones that translate (the hull's bob, the recoil on `fire`) drift and
    nothing else complains.
    """
    scene = bpy.context.scene
    depsgraph = bpy.context.evaluated_depsgraph_get()

    expected = {b["name"]: b["pivot"] for b in rig["bones"]}
    worst = 0.0
    worst_at = None
    checked = 0

    # make_action stashes each action in its own NLA track, and every one of
    # them evaluates unless muted. Assigning animation_data.action on top of
    # 13 live strips poses the rig with all fourteen at once — which shows up
    # as bones drifting even in animations that never key them. Mute the lot;
    # the glTF exporter walks actions individually (which is what the
    # stashing is FOR), so this only concerns the check.
    for track in armature.animation_data.nla_tracks:
        track.mute = True

    for anim in rig["anims"]:
        track = armature.animation_data.nla_tracks.get(anim["name"])
        strip = track.strips[0]
        armature.animation_data.action = strip.action

        frames = sorted({k["frame"] for keys in anim["tracks"].values()
                         for k in keys})
        for frame in frames:
            scene.frame_set(frame)
            depsgraph.update()
            for bone_name, rest in expected.items():
                pbone = armature.pose.bones[bone_name]
                # matrix is the pose bone's head in armature space.
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
            "  pose-bone `location` is interpreted in."
            % (worst_at[0], worst_at[1], worst_at[2], worst, rig["euler_order"]))
    print("  selftest OK: %d bone-poses, worst delta %.3g" % (checked, worst))


def _expected_head(rig, anim, bone_name, frame):
    """Where the source rig says this bone's head is, at this frame.

    A direct transcription of the generator's own pose walk: rotate about the
    bone's pivot, translate, concatenate onto the parent. Parents precede
    children in `bones`, so one pass is enough.
    """
    world = {}
    for b in rig["bones"]:
        rx, ry, rz, tx, ty, tz = _sample(anim["tracks"].get(b["name"]), frame)
        piv = b["pivot"]
        rot = _euler_matrix(rx, ry, rz, rig["euler_order"])
        origin = [piv[i] + (tx, ty, tz)[i]
                  - sum(rot[i][k] * piv[k] for k in range(3)) for i in range(3)]
        if b["parent"] is None:
            world[b["name"]] = (rot, origin)
        else:
            prot, porigin = world[b["parent"]]
            world[b["name"]] = (
                [[sum(prot[i][k] * rot[k][j] for k in range(3))
                  for j in range(3)] for i in range(3)],
                [porigin[i] + sum(prot[i][k] * origin[k] for k in range(3))
                 for i in range(3)],
            )
    rot, origin = world[bone_name]
    piv = dict((b["name"], b["pivot"]) for b in rig["bones"])[bone_name]
    return [origin[i] + sum(rot[i][k] * piv[k] for k in range(3))
            for i in range(3)]


def _sample(keys, f):
    if not keys:
        return (0.0,) * 6
    flat = [(k["frame"], *k["rot"], *k["pos"]) for k in keys]
    if f <= flat[0][0]:
        return tuple(flat[0][1:])
    if f >= flat[-1][0]:
        return tuple(flat[-1][1:])
    for a, b in zip(flat, flat[1:]):
        if a[0] <= f <= b[0]:
            t = (f - a[0]) / float(b[0] - a[0]) if b[0] != a[0] else 0.0
            return tuple(a[1 + k] + (b[1 + k] - a[1 + k]) * t for k in range(6))
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
    # Blender order 'XZY' composes as Ry·Rz·Rx — rightmost axis applied first.
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

    print("── centaur: %d bones, %d anims, %d verts ──"
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
