#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""mc_rig_export.py — the machine centaur's rig, out of Python and into JSON.

    python3 tools/mc_rig_export.py --out assets/rig/machine_centaur.json
    python3 tools/mc_rig_export.py --verify        # no output, just the check

── Why this exists ────────────────────────────────────────────────────────
`reference/machine_centaur.h` already contains the whole rig — 23 bones, 28
limb display lists, 13 animations. But it contains it as F3DEX2: the bone
table's payload is `const Gfx *dl[3]`, and this engine does not consume
display lists (geometry goes through gltf_to_t3d into Tiny3D, and the display
list is built on the RSP by Tiny3D's own microcode).

What IS portable is everything else: the hierarchy, the pivots, and the
keyframes. This script lifts those out, and `tools/blender/centaur.py` rebuilds
them as an armature so `gltf_to_t3d` emits real animation data.

It imports `machine_centaur_gen` rather than parsing the generated header, for
the reason `tools/blender/anim_io.py` gives about the goblin's actions:

    They are not data; they are code.

`build_bones()` and `build_anims()` are pure — they touch neither the mesh nor
the filesystem, and the module is `if __name__ == "__main__"` guarded — so
importing it costs nothing and cannot drift from what the real generator does.

── The coordinate change, and why it is verified rather than argued ───────
The generator works in centimetres, Y up, Z forward. Blender is Z up. The
conversion is (x, y, z) -> (x, -z, y), and it is deliberately NOT (x, z, y):

    "(x,y,z) -> (x,z,y) is a reflection, not a rotation. It flips handedness
     and inverts every normal. The hellhound's skull was pointing backwards
     out of its own neck."   -- docs/VEIL_DESIGN.md §9

Rotations have to move with the axes, and the Euler ORDER moves too. Under
M: (x,y,z) -> (x,-z,y), a generator rotation R = Rz·Ry·Rx becomes

    M R M^-1 = Ry(-rz) · Rz(ry) · Rx(rx)

which is Blender Euler order 'XZY' with the components (rx, -rz, ry). That is
easy to state and just as easy to get subtly wrong, so `--verify` does not
trust it: it evaluates every bone of every animation at every keyframe under
BOTH conventions and asserts the world-space bone positions agree to 1e-6
after mapping. Same discipline `tools/blender/quake_map.py` uses — no `bpy`
import anywhere in this file, so the maths is testable with a bare python3.
"""

import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import machine_centaur_gen as mc  # noqa: E402  (path set up above)

# The animation frame rate. Not recorded in the rig itself — mc_anim_render.py
# defaults to 20, and the timings (a 48-frame walk stride, a 96-frame idle
# settle) only read correctly at that rate.
DEFAULT_FPS = 20

# Centimetres -> Blender units. 1 BU = 1 m puts the centaur at ~2.5 BU, which
# is the range m64lib's other characters occupy; mkModel's baseScale takes it
# from there to world units.
UNIT_SCALE = 0.01

# Blender Euler order equivalent to the generator's Rz·Ry·Rx after the axis
# change. Derived above, asserted by --verify.
BLENDER_EULER_ORDER = "XZY"


# ── Pure 4x4 maths (mirrors machine_centaur_gen.pose_matrices, sans numpy) ──
def _mat_ident():
    return [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def _mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)]
            for i in range(4)]


def _rot_xyz(rx, ry, rz):
    """R = Rz·Ry·Rx, angles in degrees — the generator's convention."""
    cx, cy, cz = (math.cos(math.radians(v)) for v in (rx, ry, rz))
    sx, sy, sz = (math.sin(math.radians(v)) for v in (rx, ry, rz))
    return [
        [cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx],
        [sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx],
        [-sy,     cy * sx,                cy * cx],
    ]


def _rot_xzy(rx, ry, rz):
    """R = Ry·Rz·Rx, angles in degrees — Blender Euler order 'XZY'."""
    cx, cy, cz = (math.cos(math.radians(v)) for v in (rx, ry, rz))
    sx, sy, sz = (math.sin(math.radians(v)) for v in (rx, ry, rz))
    Rx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    Ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    Rz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    m = [[sum(Ry[i][k] * Rz[k][j] for k in range(3)) for j in range(3)]
         for i in range(3)]
    return [[sum(m[i][k] * Rx[k][j] for k in range(3)) for j in range(3)]
            for i in range(3)]


def _local(rot3, pivot, trans):
    """Rotate about `pivot`, then translate — the generator's local matrix."""
    L = _mat_ident()
    for i in range(3):
        for j in range(3):
            L[i][j] = rot3[i][j]
    for i in range(3):
        rp = sum(rot3[i][k] * pivot[k] for k in range(3))
        L[i][3] = pivot[i] + trans[i] - rp
    return L


def _sample(keys, f):
    """Linear interpolation between keyframes, clamped — matches the
    generator's sample_track so a verify failure means the CONVERSION is
    wrong, not the sampling."""
    if f <= keys[0][0]:
        return tuple(keys[0][1:])
    if f >= keys[-1][0]:
        return tuple(keys[-1][1:])
    for a, b in zip(keys, keys[1:]):
        if a[0] <= f <= b[0]:
            t = (f - a[0]) / float(b[0] - a[0]) if b[0] != a[0] else 0.0
            return tuple(a[1 + k] + (b[1 + k] - a[1 + k]) * t for k in range(6))
    return tuple(keys[-1][1:])


def _world(bones, order, anim, f, converted):
    """name -> 4x4 world matrix at frame `f`, in either convention."""
    out = {}
    for name in order:
        b = bones[name]
        keys = anim["tracks"].get(name)
        rx, ry, rz, tx, ty, tz = _sample(keys, f) if keys else (0.0,) * 6
        if converted:
            # Call the real conversion functions, never a copy of them —
            # a verifier that reimplements what it checks can agree with
            # itself while both are wrong.
            rot = _rot_xzy(*cv_euler(rx, ry, rz))
            piv = cv_point(b["pivot"])
            tr = cv_point((tx, ty, tz))
        else:
            rot = _rot_xyz(rx, ry, rz)
            piv = tuple(b["pivot"])
            tr = (tx, ty, tz)
        L = _local(rot, piv, tr)
        parent = b["parent"]
        out[name] = L if parent is None else _mat_mul(out[parent], L)
    return out


# ── The conversion ─────────────────────────────────────────────────────────
def cv_point(p):
    """Generator space (cm, Y up) -> Blender space (cm, Z up).

    (x, y, z) -> (x, -z, y). A rotation, NOT the reflection (x, z, y) that
    docs/VEIL_DESIGN.md §9 caught pointing a skull backwards out of its neck.
    """
    return (p[0], -p[2], p[1])


def cv_euler(rx, ry, rz):
    """Generator Euler (Rz·Ry·Rx, degrees) -> Blender 'XZY' Euler, degrees."""
    return (rx, -rz, ry)


def load_rig():
    """Build the rig, idempotently.

    `mc.add_bone` APPENDS to the module-global BONE_ORDER, so calling
    build_bones() twice in one process silently gives 46 bones instead of 23
    — the dict is overwritten but the order list is not. Every bone appears
    twice, poses identically, and nothing downstream complains until an
    exporter walks the list. Clearing both first makes the call idempotent;
    the assert is there because the failure is invisible otherwise.
    """
    mc.BONES.clear()
    del mc.BONE_ORDER[:]
    mc.build_bones()
    assert len(mc.BONE_ORDER) == len(set(mc.BONE_ORDER)), \
        "duplicate bones in BONE_ORDER — build_bones() ran more than once"
    return mc.build_anims()


def load_mesh():
    """Run the generator's mesh build, minus every side effect.

    This mirrors the body of `machine_centaur_gen.main()` exactly — same
    calls, same order, same lean pass — with `build_textures` and the three
    `write_*` emitters left out. Textures need PIL and produce PNGs this
    pipeline does not consume (materials come from tools/f3d_inject.py), and
    the writers produce the F3DEX2 output this whole script exists to avoid.

    Kept as an explicit transcription rather than a call into main(), because
    main() parses argv and writes files; there is no seam in it to reuse.
    """
    mc.M.__init__()  # module-global accumulator — reset so this is idempotent

    with mc.bone("hull"):
        mc.build_hull()
    for side in (1, -1):
        for pair in ("front", "rear"):
            mc.build_leg(side, pair)

    # Everything from here up is the part of him that used to be a person.
    human_mark = len(mc.M.v)

    with mc.bone("torso"):
        seam = mc.build_graft()
        mc.build_torso(seam)
        mc.build_spine()
        mc.build_head()
        mc.build_right_arm()
        mc.build_shotgun_arm()

    # The lean is a post-pass over the human half only — see main().
    for i in range(human_mark, len(mc.M.v)):
        x, y, z = mc.M.v[i]
        mc.M.v[i] = (x, y, z + mc.lean_z(y))

    verts = [[c * UNIT_SCALE for c in cv_point(p)] for p in mc.M.v]
    # Materials become one Blender mesh each: gltf_to_t3d keys its material
    # table by name and silently drops primitives whose material is missing.
    prims = {}
    for (bone_name, mat), tris in mc.M.groups.items():
        prims.setdefault(mat, []).extend(tris)

    # Drop exactly-duplicated faces. F3DEX2 tolerates them (it just draws
    # both, z-fighting); Blender's mesh validate() rejects the mesh outright,
    # so the whole material silently fails to build. The canonical form
    # rotates each triple so its smallest index leads, which means a
    # REVERSED-winding twin canonicalises differently and is preserved — that
    # one is an intentional double-sided face, not a mistake.
    dropped = 0
    for mat, tris in prims.items():
        seen, kept = set(), []
        for tri in tris:
            i = tri.index(min(tri))
            canon = (tri[i], tri[(i + 1) % 3], tri[(i + 2) % 3])
            if canon in seen:
                dropped += 1
                continue
            seen.add(canon)
            kept.append(tri)
        prims[mat] = kept
    if dropped:
        print("  dropped %d duplicate face(s) from the source mesh" % dropped)

    return {
        "verts": verts,
        "colors": [list(c) for c in mc.M.c],
        "uvs": [list(uv) for uv in mc.M.uv],
        # One bone per vertex. The generator records this at add() time, so
        # a triangle that spans a seam still binds each of its vertices to
        # the bone that owns it — which is what Tiny3D's rigid skinning wants
        # (one bone per vertex, at most three bones per triangle).
        "vert_bone": list(mc.M.b),
        "prims": {mat: [list(t) for t in tris] for mat, tris in prims.items()},
    }


def build():
    anims = load_rig()

    bones = [{
        "name": name,
        "parent": mc.BONES[name]["parent"],
        "pivot": [c * UNIT_SCALE for c in cv_point(mc.BONES[name]["pivot"])],
    } for name in mc.BONE_ORDER]

    out_anims = []
    for name, a in anims.items():
        tracks = {}
        for bone_name, keys in a["tracks"].items():
            tracks[bone_name] = [{
                "frame": int(k[0]),
                "rot": list(cv_euler(k[1], k[2], k[3])),
                "pos": [c * UNIT_SCALE for c in cv_point((k[4], k[5], k[6]))],
            } for k in keys]
        out_anims.append({
            "name": name,
            "length": a["frames"],
            "loop": bool(a["loop"]),
            "tracks": tracks,
        })

    return {
        "source": "machine_centaur_gen.py",
        "euler_order": BLENDER_EULER_ORDER,
        "unit_scale": UNIT_SCALE,
        "fps": DEFAULT_FPS,
        "bones": bones,
        "anims": out_anims,
        "mesh": load_mesh(),
    }


# ── Verification ───────────────────────────────────────────────────────────
def verify(tol=1e-6):
    """Assert the converted rig poses identically to the generator's own.

    For every animation, at every keyframe of every track, evaluate all 23
    bones under both conventions and compare world-space bone origins after
    mapping. Any error in cv_point, cv_euler or BLENDER_EULER_ORDER moves a
    limb, so this catches all three.
    """
    anims = load_rig()

    checked = 0
    worst = 0.0
    for name, a in anims.items():
        frames = sorted({k[0] for keys in a["tracks"].values() for k in keys})
        # Half-frames too: the conversion must hold between keys, not only on
        # them, or a wrong Euler order hides wherever the keys happen to be 0.
        frames += [f + 0.5 for f in frames[:-1]]
        for f in frames:
            ref = _world(mc.BONES, mc.BONE_ORDER, a, f, converted=False)
            got = _world(mc.BONES, mc.BONE_ORDER, a, f, converted=True)
            for bone_name in mc.BONE_ORDER:
                expect = cv_point([ref[bone_name][i][3] for i in range(3)])
                actual = [got[bone_name][i][3] for i in range(3)]
                for e, g in zip(expect, actual):
                    d = abs(e - g)
                    worst = max(worst, d)
                    if d > tol:
                        raise SystemExit(
                            "VERIFY FAILED: %s/%s frame %s: %.9f vs %.9f "
                            "(delta %.3g)\n"
                            "  The axis change, the Euler order, or both are "
                            "wrong. Do not build a rig on this."
                            % (name, bone_name, f, e, g, d))
                checked += 1
    print("verify OK: %d bone-poses across %d anims, worst delta %.3g"
          % (checked, len(anims), worst))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()

    verify()  # always — an unverified rig is not worth writing
    if args.verify and not args.out:
        return

    if not args.out:
        ap.error("--out is required unless --verify is given alone")

    data = build()
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(data, f, indent=1, sort_keys=True)
    mesh = data["mesh"]
    tris = sum(len(t) for t in mesh["prims"].values())
    print("%s: %d bones, %d anims (%d tracks), %d fps, %d verts / %d tris "
          "in %d materials"
          % (args.out, len(data["bones"]), len(data["anims"]),
             sum(len(a["tracks"]) for a in data["anims"]), data["fps"],
             len(mesh["verts"]), tris, len(mesh["prims"])))

    unknown = set(mesh["vert_bone"]) - {b["name"] for b in data["bones"]}
    if unknown:
        raise SystemExit("mesh binds to bones the rig does not define: %s"
                         % sorted(unknown))


if __name__ == "__main__":
    main()
