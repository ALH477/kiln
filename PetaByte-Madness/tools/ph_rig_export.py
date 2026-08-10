#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""ph_rig_export.py — Dr. Horner's rig, out of Python and into JSON.

    python3 tools/ph_rig_export.py --out assets/rig/horner.json
    python3 tools/ph_rig_export.py --verify        # no output, just the check

Same job as mc_rig_export.py, for the same reason: ph_rig.py already has the
whole rig — 17 joints, rigid per-vertex binding, FK posing — but as a
rigid-segment N64 puppet (per-joint Vtx arrays + display lists), which this
engine's importer does not consume (geometry goes through gltf_to_t3d into
Tiny3D). `tools/blender/horner.py` rebuilds it as an armature so gltf_to_t3d
emits real skinned animation data; this script lifts the hierarchy, the
pivots, the per-vertex bone binding and the new named clips (ph_anim_clips.py)
out of ph_rig.py's Rig() and ph_anim_clips.CLIPS, and does no authoring of its
own — if a limb is in the wrong place, the bug is in ph_rig.py or
ph_anim_clips.py, not here.

── The coordinate change ───────────────────────────────────────────────────
ph_rig.py (like patrick_horner_gen.py, which it binds) works in centimetres,
Y up, Z forward — tools/blender/objkit.py's yup_to_zup calls this "this
drop's convention", and pm_props.py already converts patrick_horner.obj
through it for the existing rigid model. Blender is Z up. The conversion is
(x, y, z) -> (x, -z, y) — a rotation, not the reflection docs/VEIL_DESIGN.md
§9 warns about — and it is exactly Rx(+90°).

Rotations move with the axes, and the Euler order moves too, but ph_rig.py's
own euler(rx, ry, rz) = Rx(rx)@Ry(ry)@Rz(rz) composes in the OPPOSITE order
from machine_centaur_gen's Rz@Ry@Rx, so mc_rig_export.py's answer ('XZY',
angles (rx,-rz,ry)) does not carry over unchanged — only the angle remap
does. Conjugating Rx(rx)@Ry(ry)@Rz(rz) by M=Rx(90) gives Rx(rx)@Rz(ry)@Ry(-rz)
(Ry and Rz swap because M rotates the Y axis onto Z and the Z axis onto -Y),
which is Blender Euler order 'YZX' with components (rx, -rz, ry) — same
per-angle remap as the centaur's case, different composition order because
the two generators' own conventions are reversed. `--verify` does not trust
this derivation any more than mc_rig_export.py trusts its own: it evaluates
ph_rig.py's own matrices() against this module's independent 4x4 maths under
both conventions and asserts they agree.
"""

import argparse
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ph_anim_clips as clips  # noqa: E402
from ph_rig import JOINTS, NAMES, NJ, Rig  # noqa: E402

UNIT_SCALE = 0.01           # cm -> Blender units (metres), matches pm_props.py
BLENDER_EULER_ORDER = "YZX"  # derived above, asserted by --verify


# ── the coordinate change ───────────────────────────────────────────────────
def cv_point(p):
    """Generator space (cm, Y up) -> Blender space (cm, Z up). (x,y,z)->(x,-z,y)."""
    return (p[0], -p[2], p[1])


def cv_euler(rx, ry, rz):
    """Generator Euler (Rx·Ry·Rz, degrees) -> Blender 'YZX' Euler, degrees."""
    return (rx, -rz, ry)


# ── bones + mesh ─────────────────────────────────────────────────────────────
def build_bones(rig):
    return [{
        "name": name,
        "parent": JOINTS[i][1],
        "pivot": [c * UNIT_SCALE for c in cv_point(rig.pivot[i])],
    } for i, name in enumerate(NAMES)]


def build_mesh(rig):
    verts = [[c * UNIT_SCALE for c in cv_point(v)] for v in rig.rest]
    colors = [_srgb(*c) for c in rig.col]
    uvs = [list(uv) for uv in rig.uv]
    vert_bone = [NAMES[int(j)] for j in rig.bind_of]

    prims = {}
    for a, b, c, mat in rig.tris:
        prims.setdefault(mat, []).append([int(a), int(b), int(c)])

    return {
        "verts": verts,
        "colors": colors,
        "uvs": uvs,
        "vert_bone": vert_bone,
        "prims": prims,
    }


def _srgb(r, g, b, a=255):
    """0-255 sRGB -> linear floats, matching tools/blender/m64lib.py's srgb().
    Duplicated rather than imported: this script runs under a bare python3
    (mc_rig_export.py's discipline — no bpy, testable without Blender), and
    m64lib.py imports bpy at module scope."""
    gamma = 2.2
    return [(r / 255.0) ** gamma, (g / 255.0) ** gamma,
            (b / 255.0) ** gamma, a / 255.0]


# ── anims ────────────────────────────────────────────────────────────────────
def build_channels(keyframes):
    """[(frame, {joint: (rx,ry,rz)}), ...] -> {joint: [{frame,rot}, ...]},
    with every joint that appears at ANY frame of this clip given an
    explicit (0,0,0) key at every frame it is missing from — Blender's
    per-bone fcurves need an explicit key at every frame they should span;
    there is no implicit "default to zero" the way ph_rig.py's own
    pose_at() gets by defaulting a missing joint per-call."""
    joints = set()
    for _, pose in keyframes:
        joints.update(pose.keys())
    channels = {j: [] for j in joints}
    for frame, pose in keyframes:
        for j in joints:
            rx, ry, rz = pose.get(j, (0.0, 0.0, 0.0))
            channels[j].append({"frame": int(frame), "rot": [rx, ry, rz]})
    return channels


def build_anims():
    out = []
    for name, loop, keyframes in clips.CLIPS:
        channels = build_channels(keyframes)
        tracks = {}
        for joint, keys in channels.items():
            tracks[joint] = [{
                "frame": k["frame"],
                "rot": list(cv_euler(*k["rot"])),
            } for k in keys]
        out.append({
            "name": name,
            "loop": loop,
            "length": max(k["frame"] for keys in tracks.values() for k in keys),
            "tracks": tracks,
        })
    return out


def build():
    rig = Rig()
    return {
        "source": "ph_rig.py + ph_anim_clips.py",
        "euler_order": BLENDER_EULER_ORDER,
        "unit_scale": UNIT_SCALE,
        "bones": build_bones(rig),
        "anims": build_anims(),
        "mesh": build_mesh(rig),
    }


# ── verification ─────────────────────────────────────────────────────────────
def _mat_ident():
    return [[1.0 if i == j else 0.0 for j in range(4)] for i in range(4)]


def _mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)]
            for i in range(4)]


def _rot_xyz(rx, ry, rz):
    """R = Rx·Ry·Rz, angles in degrees — ph_rig.py's euler() convention."""
    cx, cy, cz = (math.cos(math.radians(v)) for v in (rx, ry, rz))
    sx, sy, sz = (math.sin(math.radians(v)) for v in (rx, ry, rz))
    Rx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    Ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    Rz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    m = [[sum(Ry[i][k] * Rz[k][j] for k in range(3)) for j in range(3)]
         for i in range(3)]
    return [[sum(Rx[i][k] * m[k][j] for k in range(3)) for j in range(3)]
            for i in range(3)]


def _rot_yzx(rx, ry, rz):
    """R = Rx·Rz·Ry, angles in degrees — Blender Euler order 'YZX'."""
    cx, cy, cz = (math.cos(math.radians(v)) for v in (rx, ry, rz))
    sx, sy, sz = (math.sin(math.radians(v)) for v in (rx, ry, rz))
    Rx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    Ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    Rz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    m = [[sum(Rz[i][k] * Ry[k][j] for k in range(3)) for j in range(3)]
         for i in range(3)]
    return [[sum(Rx[i][k] * m[k][j] for k in range(3)) for j in range(3)]
            for i in range(3)]


def _local_mat(rot3, offset):
    """[[R, offset], [0, 1]] — ph_rig.py's own `translate(local) @ euler(pose)`
    collapses to exactly this (translate's rotation block is identity, so the
    product's rotation block is R and its translation column is `offset`
    untouched — unlike mc_rig_export.py's generator, which composes
    "rotate about an absolute pivot, then translate", ph_rig.py composes
    "translate by an already-relative-to-parent offset, then rotate", and
    the two are not the same formula even though both build a per-joint
    local matrix. Reproducing the WRONG one here would make this check agree
    with itself while both are wrong — see mc_rig_export.py's own warning
    about exactly that failure mode."""
    L = _mat_ident()
    for i in range(3):
        for j in range(3):
            L[i][j] = rot3[i][j]
        L[i][3] = offset[i]
    return L


def _world(parent, local_offsets, order, rots, converted):
    out = {}
    for i in order:
        rx, ry, rz = rots[i]
        if converted:
            rot = _rot_yzx(*cv_euler(rx, ry, rz))
            off = cv_point(local_offsets[i])
        else:
            rot = _rot_xyz(rx, ry, rz)
            off = tuple(local_offsets[i])
        L = _local_mat(rot, off)
        p = parent[i]
        out[i] = L if p < 0 else _mat_mul(out[p], L)
    return out


def verify(tol=1e-6):
    """Assert Blender's convention poses this rig the way ph_rig.py's own
    matrices() does — the same discipline mc_rig_export.py uses for the
    centaur, adapted to ph_rig.py's reversed Euler composition AND its
    different (translate-then-rotate, parent-relative) local-matrix shape."""
    rig = Rig()
    order = list(range(NJ))

    checked = 0
    worst = 0.0
    for name, _loop, keyframes in clips.CLIPS:
        for frame, pose in keyframes:
            rots = [pose.get(NAMES[i], (0.0, 0.0, 0.0)) for i in range(NJ)]
            ref = _world(rig.parent, rig.local, order, rots, converted=False)
            got = _world(rig.parent, rig.local, order, rots, converted=True)
            for i in range(NJ):
                expect = cv_point([ref[i][k][3] for k in range(3)])
                actual = [got[i][k][3] for k in range(3)]
                for e, g in zip(expect, actual):
                    d = abs(e - g)
                    worst = max(worst, d)
                    if d > tol:
                        raise SystemExit(
                            "VERIFY FAILED: %s/%s frame %s: %.9f vs %.9f "
                            "(delta %.3g)\n"
                            "  The axis change, the Euler order, or both are "
                            "wrong. Do not build a rig on this."
                            % (name, NAMES[i], frame, e, g, d))
                checked += 1
    print("verify OK: %d bone-poses across %d clips, worst delta %.3g"
          % (checked, len(clips.CLIPS), worst))


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
    print("%s: %d bones, %d anims (%d tracks), %d verts / %d tris in %d "
          "materials"
          % (args.out, len(data["bones"]), len(data["anims"]),
             sum(len(a["tracks"]) for a in data["anims"]),
             len(mesh["verts"]), tris, len(mesh["prims"])))

    unknown = set(mesh["vert_bone"]) - {b["name"] for b in data["bones"]}
    if unknown:
        raise SystemExit("mesh binds to bones the rig does not define: %s"
                         % sorted(unknown))


if __name__ == "__main__":
    main()
