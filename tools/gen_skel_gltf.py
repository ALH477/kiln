#!/usr/bin/env python3
"""Generate a minimal 2-bone skinned glTF: a "hip" box and an "arm" box
hinged above it, plus two rotation-only animations on the arm bone — "idle"
(a slight constant tilt) and "swing" (a back-and-forth wave) — so a demo can
exercise kiln_skel's two-slot blend the way Tiny3D's own examples blend
idle/walk. This is the smallest rig that exercises gltf_to_t3d's skin +
animation path (rigid one-bone-per-vertex skinning, quaternion rotation
keyframes) without needing fast64 custom properties — --ignore-materials
(same as assets/cube.gltf) skips those.

Bone layout (bind pose, glTF units before mkModel's base-scale):
  hip  (root)  at (0, 0, 0)
  arm  (child) at (0, 1, 0) relative to hip

Mesh: two boxes, each rigidly skinned to one bone. The hip box spans local
Y [-0.5, 0.5]; the arm box spans world Y [1.0, 2.0] (bind space) so that
after pre-multiplying by the arm bone's inverse bind pose it sits at local
Y [0, 1] — the swing pivots at the arm's bottom edge, like a hinge.
"""

import base64
import struct
import sys

Vec3 = tuple


def box_verts(center, half):
    """8 corners of an axis-aligned box, CCW-friendly winding shared with
    assets/cube.gltf's CUBE_TRIS layout."""
    cx, cy, cz = center
    hx, hy, hz = half
    return [
        (cx - hx, cy - hy, cz - hz),
        (cx + hx, cy - hy, cz - hz),
        (cx + hx, cy + hy, cz - hz),
        (cx - hx, cy + hy, cz - hz),
        (cx - hx, cy - hy, cz + hz),
        (cx + hx, cy - hy, cz + hz),
        (cx + hx, cy + hy, cz + hz),
        (cx - hx, cy + hy, cz + hz),
    ]


def build_box(center, half, bone_index, base_index):
    """Returns (positions, normals, joints, weights, indices) for one box,
    duplicating verts per-face so flat normals work, matching cube.gltf."""
    corners = box_verts(center, half)
    positions, normals, joints, weights, indices = [], [], [], [], []
    # 24 verts total: one per (corner, face) pair, so each face gets its own
    # flat normal (matches examples/actors-demo's make_color_cube approach).
    face_corner_idx = [
        (0, 1, 2, 3),  # -Z
        (5, 4, 7, 6),  # +Z
        (0, 4, 5, 1),  # -Y
        (3, 2, 6, 7),  # +Y
        (4, 0, 3, 7),  # -X
        (1, 5, 6, 2),  # +X
    ]
    face_normals = [(0, 0, -1), (0, 0, 1), (0, -1, 0), (0, 1, 0), (-1, 0, 0), (1, 0, 0)]
    vert_map = {}
    for face_i, quad in enumerate(face_corner_idx):
        n = face_normals[face_i]
        local_idx = []
        for c in quad:
            positions.append(corners[c])
            normals.append(n)
            joints.append((bone_index, 0, 0, 0))
            weights.append((1.0, 0.0, 0.0, 0.0))
            local_idx.append(len(positions) - 1)
        a, b, c2, d = local_idx
        indices.append((base_index + a, base_index + b, base_index + c2))
        indices.append((base_index + c2, base_index + d, base_index + a))
    return positions, normals, joints, weights, indices


def quat_axis_angle(axis, deg):
    import math
    rad = math.radians(deg) * 0.5
    s = math.sin(rad)
    ax, ay, az = axis
    return (ax * s, ay * s, az * s, math.cos(rad))


def gltf_make(outpath):
    hip_center, hip_half = (0.0, 0.0, 0.0), (0.4, 0.5, 0.4)
    arm_center, arm_half = (0.0, 1.5, 0.0), (0.3, 0.5, 0.3)

    hp, hn, hj, hw, hi = build_box(hip_center, hip_half, 0, 0)
    ap, an, aj, aw, ai = build_box(arm_center, arm_half, 1, len(hp))

    positions = hp + ap
    normals = hn + an
    joints = hj + aj
    weights = hw + aw
    indices = hi + ai

    pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
    norm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
    joint_bytes = b"".join(struct.pack("<4B", *j) for j in joints)
    weight_bytes = b"".join(struct.pack("<4f", *w) for w in weights)
    idx_flat = [i for tri in indices for i in tri]
    idx_bytes = b"".join(struct.pack("<H", i) for i in idx_flat)

    def pad4(b):
        return b + b"\x00" * ((4 - len(b) % 4) % 4)

    pos_bytes, norm_bytes, joint_bytes, weight_bytes, idx_bytes = (
        pad4(pos_bytes), pad4(norm_bytes), pad4(joint_bytes), pad4(weight_bytes), pad4(idx_bytes)
    )

    buf = pos_bytes + norm_bytes + joint_bytes + weight_bytes + idx_bytes
    off_pos = 0
    off_norm = off_pos + len(pos_bytes)
    off_joint = off_norm + len(norm_bytes)
    off_weight = off_joint + len(joint_bytes)
    off_idx = off_weight + len(weight_bytes)

    xs = [p[0] for p in positions]
    ys = [p[1] for p in positions]
    zs = [p[2] for p in positions]

    # ── Animations, both rotating the arm bone around Z ─────────────────
    # "idle": near-static, a small constant tilt so it's visually distinct
    # from bind pose. "swing": a +-30 degree wave. Both get their own
    # time/rotation accessor pair, packed into one animation buffer.
    anim_defs = [
        ("idle", [0.0, 1.0], [quat_axis_angle((0, 0, 1), -5.0)] * 2),
        ("swing", [0.0, 0.5, 1.0, 1.5], [
            quat_axis_angle((0, 0, 1), 0.0),
            quat_axis_angle((0, 0, 1), 30.0),
            quat_axis_angle((0, 0, 1), -30.0),
            quat_axis_angle((0, 0, 1), 0.0),
        ]),
    ]

    anim_chunks = []
    for name, times, rots in anim_defs:
        time_bytes = pad4(b"".join(struct.pack("<f", t) for t in times))
        rot_bytes = pad4(b"".join(struct.pack("<4f", *q) for q in rots))
        anim_chunks.append((name, times, rots, time_bytes, rot_bytes))

    anim_buf = b"".join(tb + rb for _, _, _, tb, rb in anim_chunks)
    anim_offsets = []
    cursor = 0
    for _, _, _, tb, rb in anim_chunks:
        anim_offsets.append((cursor, len(tb), cursor + len(tb), len(rb)))
        cursor += len(tb) + len(rb)

    def b64(data):
        return base64.b64encode(data).decode("ascii")

    gltf = {
        "asset": {"version": "2.0", "generator": "Kiln test skel asset generator"},
        "scene": 0,
        "scenes": [{"nodes": [0, 2]}],
        "nodes": [
            {"name": "hip", "translation": [0.0, 0.0, 0.0], "children": [1]},
            {"name": "arm", "translation": [0.0, 1.0, 0.0]},
            {"name": "SkelMesh", "mesh": 0},
        ],
        "skins": [{"joints": [0, 1]}],
        "animations": [
            {
                "name": name,
                # Accessor 4 is INDICES; each animation's time/rotation pair
                # follows after that, two accessors apiece, in anim_defs order.
                "samplers": [{"input": 5 + i * 2, "interpolation": "LINEAR", "output": 6 + i * 2}],
                "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}}],
            }
            for i, (name, _, _, _, _) in enumerate(anim_chunks)
        ],
        "meshes": [{
            "name": "SkelMesh",
            "primitives": [{
                "attributes": {
                    "POSITION": 0, "NORMAL": 1, "JOINTS_0": 2, "WEIGHTS_0": 3,
                },
                "indices": 4,
                "material": 0,
            }],
        }],
        "materials": [{
            "name": "SkelMat",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.9, 0.6, 0.2, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.8,
            },
        }],
        "accessors": [
            {  # 0 POSITION
                "bufferView": 0, "componentType": 5126, "count": len(positions),
                "type": "VEC3",
                "min": [min(xs), min(ys), min(zs)],
                "max": [max(xs), max(ys), max(zs)],
            },
            {  # 1 NORMAL
                "bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3",
            },
            {  # 2 JOINTS_0
                "bufferView": 2, "componentType": 5121, "count": len(joints), "type": "VEC4",
            },
            {  # 3 WEIGHTS_0
                "bufferView": 3, "componentType": 5126, "count": len(weights), "type": "VEC4",
            },
            {  # 4 indices
                "bufferView": 4, "componentType": 5123, "count": len(idx_flat), "type": "SCALAR",
            },
        ] + [
            acc
            for i, (name, times, rots, tb, rb) in enumerate(anim_chunks)
            for acc in (
                {  # anim input (time)
                    "bufferView": 5 + i * 2, "componentType": 5126, "count": len(times),
                    "type": "SCALAR", "min": [min(times)], "max": [max(times)],
                },
                {  # anim output (rotation)
                    "bufferView": 6 + i * 2, "componentType": 5126, "count": len(rots), "type": "VEC4",
                },
            )
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": off_pos, "byteLength": len(pos_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": off_norm, "byteLength": len(norm_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": off_joint, "byteLength": len(joint_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": off_weight, "byteLength": len(weight_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": off_idx, "byteLength": len(idx_bytes), "target": 34963},
        ] + [
            bv
            for (t_off, t_len, r_off, r_len) in anim_offsets
            for bv in (
                {"buffer": 1, "byteOffset": t_off, "byteLength": t_len},
                {"buffer": 1, "byteOffset": r_off, "byteLength": r_len},
            )
        ],
        "buffers": [
            {"byteLength": len(buf), "uri": "data:application/octet-stream;base64," + b64(buf)},
            {"byteLength": len(anim_buf), "uri": "data:application/octet-stream;base64," + b64(anim_buf)},
        ],
    }

    import json
    with open(outpath, "w") as f:
        json.dump(gltf, f, indent=1)


if __name__ == "__main__":
    gltf_make(sys.argv[1] if len(sys.argv) > 1 else "skel_test.gltf")
