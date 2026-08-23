# SPDX-License-Identifier: MIT
"""objkit.py — a Wavefront OBJ reader for art dropped in as .obj rather than
authored through kilnlib.

    python3 tools/blender/objkit.py <file.obj>     # parse and report

Deliberately NOT `bpy.ops.wm.obj_import`. Two reasons, both learned from real
drops this has had to read:

  * **Vertex colours.** Some OBJ exporters carry them as three extra floats
    on each `v` line — the "extended" OBJ convention, used to bring in baked
    AO or a per-facet colour bias authored outside Blender entirely. Whether
    a given Blender release imports those, and into which attribute, is
    exactly the kind of thing that silently changes under a nixpkgs bump and
    comes out as a grey model.
  * **Materials.** gltf_to_t3d keys its material table by name and silently
    DROPS primitives whose material is missing (kilnlib.make_material's note).
    The importer's naming is its own business; this way the names are ours.

Everything here is plain Python with no `bpy` import, so it is testable with a
bare `python3` — the discipline tools/blender/quake_map.py credits with
catching two real bugs before Blender ever ran.
"""

import os
import sys


def load_mtl(path):
    """name -> (r, g, b) diffuse, 0..1. Missing file returns {}."""
    out = {}
    if not os.path.exists(path):
        return out
    name = None
    for line in open(path):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "newmtl":
            name = parts[1]
        elif parts[0] == "Kd" and name:
            out[name] = tuple(float(v) for v in parts[1:4])
    return out


def _face_index(token, count):
    """One 'v', 'v/vt', 'v//vn' or 'v/vt/vn' token -> 0-based vertex index.

    OBJ indices are 1-based and may be NEGATIVE, meaning "counting back from
    the most recent vertex". Getting that wrong on a file that never uses it
    costs nothing and on a file that does costs a scrambled mesh, so it is
    handled rather than assumed absent.
    """
    i = int(token.split("/")[0])
    return i - 1 if i > 0 else count + i


def load_obj(path):
    """Parse an OBJ.

    Returns dict with:
      verts   [(x, y, z), ...]
      colors  [(r, g, b), ...] or None when the file carries none
      faces   [(i, j, k), ...]                triangulated
      face_material [str|None, ...]           parallel to faces
      face_group    [str|None, ...]           parallel to faces
    """
    verts, colors = [], []
    faces, face_material, face_group = [], [], []
    has_color = False
    material = None
    group = None

    for line in open(path):
        parts = line.split()
        if not parts:
            continue
        tag = parts[0]

        if tag == "v":
            verts.append(tuple(float(v) for v in parts[1:4]))
            if len(parts) >= 7:
                colors.append(tuple(float(v) for v in parts[4:7]))
                has_color = True
            else:
                colors.append((1.0, 1.0, 1.0))
        elif tag == "usemtl":
            material = parts[1] if len(parts) > 1 else None
        elif tag == "g":
            group = parts[1] if len(parts) > 1 else None
        elif tag == "f":
            idx = [_face_index(t, len(verts)) for t in parts[1:]]
            # Fan-triangulate. Every mesh in this drop is already triangles,
            # but a quad slipping through would otherwise be dropped silently.
            for a in range(1, len(idx) - 1):
                faces.append((idx[0], idx[a], idx[a + 1]))
                face_material.append(material)
                face_group.append(group)

    return {
        "verts": verts,
        "colors": colors if has_color else None,
        "faces": faces,
        "face_material": face_material,
        "face_group": face_group,
    }


def group_faces(obj, by="material"):
    """{key: [face, ...]} — split faces by material or group.

    One Blender mesh per key: gltf_to_t3d wants a named material per
    primitive, and keeping the split explicit means an unnamed group becomes a
    material called "default" rather than a primitive that vanishes.
    """
    key_list = obj["face_material"] if by == "material" else obj["face_group"]
    out = {}
    for face, key in zip(obj["faces"], key_list):
        out.setdefault(key or "default", []).append(face)
    return out


def load_gltf(path):
    """Parse a simple glTF into the same dict shape as load_obj.

    Scoped to what this drop actually contains: indexed TRIANGLES with
    POSITION and optional COLOR_0, one buffer, either a `data:` URI or a
    sidecar .bin. No nodes, no transforms, no skins — assert rather than
    silently ignore anything richer, because a dropped node transform is a
    model that builds fine and sits in the wrong place.
    """
    import base64
    import json
    import struct

    doc = json.load(open(path))
    buf = doc["buffers"][0]
    uri = buf.get("uri", "")
    if uri.startswith("data:"):
        blob = base64.b64decode(uri.split(",", 1)[1])
    else:
        blob = open(os.path.join(os.path.dirname(path), uri), "rb").read()

    # componentType -> (struct code, byte width)
    CTYPE = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
             5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
    NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}

    def read(index):
        acc = doc["accessors"][index]
        view = doc["bufferViews"][acc["bufferView"]]
        code, width = CTYPE[acc["componentType"]]
        n = NCOMP[acc["type"]]
        start = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        stride = view.get("byteStride") or (width * n)
        out = []
        for i in range(acc["count"]):
            off = start + i * stride
            out.append(struct.unpack_from("<" + code * n, blob, off))
        return out, acc

    verts, colors, faces, face_material = [], [], [], []
    for mesh in doc["meshes"]:
        for prim in mesh["primitives"]:
            if prim.get("mode", 4) != 4:
                raise SystemExit("objkit: %s: only TRIANGLES supported" % path)
            base = len(verts)

            pos, _ = read(prim["attributes"]["POSITION"])
            verts.extend(tuple(p) for p in pos)

            if "COLOR_0" in prim["attributes"]:
                raw, acc = read(prim["attributes"]["COLOR_0"])
                # Colours may be float or normalised integer.
                scale = {5121: 255.0, 5123: 65535.0}.get(acc["componentType"])
                for c in raw:
                    rgb = tuple(c[:3]) if scale is None else \
                        tuple(v / scale for v in c[:3])
                    colors.append(rgb)
            else:
                colors.extend([(1.0, 1.0, 1.0)] * len(pos))

            idx, _ = read(prim["indices"])
            flat = [i[0] + base for i in idx]
            for a in range(0, len(flat), 3):
                faces.append(tuple(flat[a:a + 3]))
                face_material.append(mesh.get("name") or "default")

    return {
        "verts": verts,
        "colors": colors,
        "faces": faces,
        "face_material": face_material,
        "face_group": list(face_material),
    }


def loose_parts(faces):
    """Split faces into connected components — [[face, ...], ...].

    A vegetation cluster exported as one mesh is one draw call and one
    placement. Split into its individual plants, each becomes its own named
    object in the .t3dm, and the game can scatter them with
    t3d_model_get_object + a matrix each instead of paying for the whole
    grove every time it wants one palm.
    """
    parent = {}

    def find(x):
        while parent.setdefault(x, x) != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    for face in faces:
        for i in face[1:]:
            union(face[0], i)

    buckets = {}
    for face in faces:
        buckets.setdefault(find(face[0]), []).append(face)
    # Largest first, so part_00 is the most prominent plant.
    return sorted(buckets.values(), key=len, reverse=True)


def split_double_sided(obj):
    """Make double-sided face pairs survive Blender, losslessly.

    Blender's mesh validate() rejects two faces built on the SAME set of
    vertices, whichever way they wind — and rejecting means the whole mesh is
    discarded, so one such pair silently costs an entire material. A real
    hull mesh has had two of them.

    A reversed-winding twin is not a mistake: it is a deliberately
    double-sided surface, and dropping one would make it vanish from one side
    with backface culling on. So instead of dropping anything, the second
    face gets its own copies of the vertices. Both faces survive, both
    windings survive, culling still works, and the cost is three vertices per
    twin.

    (Exactly-duplicated faces — same winding — are a different defect and are
    handled where they occur, by dropping one: a real exporter has found one
    of these in a real mesh.)

    Returns the number of faces cloned.
    """
    seen = set()
    cloned = 0
    for n, face in enumerate(obj["faces"]):
        key = frozenset(face)
        if key not in seen:
            seen.add(key)
            continue
        base = len(obj["verts"])
        for i in face:
            obj["verts"].append(obj["verts"][i])
            if obj["colors"]:
                obj["colors"].append(obj["colors"][i])
        obj["faces"][n] = (base, base + 1, base + 2)
        cloned += 1
    return cloned


def bbox(verts):
    lo = [min(v[i] for v in verts) for i in range(3)]
    hi = [max(v[i] for v in verts) for i in range(3)]
    return lo, hi


def yup_to_zup(p):
    """OBJ (Y up, this drop's convention) -> Blender (Z up).

    (x, y, z) -> (x, -z, y). NOT (x, z, y), which is a reflection: it flips
    handedness and inverts every normal. The n64-animation skill's failure-
    mode table records this exact mistake pointing a model's head backwards
    out of its own neck.
    """
    return (p[0], -p[2], p[1])


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: objkit.py <file.obj>")
    path = sys.argv[1]
    obj = load_obj(path)
    lo, hi = bbox(obj["verts"])
    print("%s: %d verts, %d tris, colors=%s"
          % (os.path.basename(path), len(obj["verts"]), len(obj["faces"]),
             "yes" if obj["colors"] else "no"))
    print("  bbox: " + "  ".join("%s %.2f..%.2f" % (ax, lo[i], hi[i])
                                 for i, ax in enumerate("XYZ")))
    for by in ("material", "group"):
        keys = group_faces(obj, by)
        print("  by %-8s %s" % (by, ", ".join(
            "%s=%d" % (k, len(v)) for k, v in sorted(keys.items()))))

    nv = len(obj["verts"])
    bad = [f for f in obj["faces"] if any(i < 0 or i >= nv for i in f)]
    degen = [f for f in obj["faces"] if len(set(f)) < 3]
    print("  out-of-range faces: %d   degenerate: %d" % (len(bad), len(degen)))


if __name__ == "__main__":
    main()
