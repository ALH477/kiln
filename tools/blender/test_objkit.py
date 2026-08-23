#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""test_objkit.py — check objkit.py's parsing and repairs without Blender.

    python3 tools/blender/test_objkit.py

objkit.py is the reader any OBJ-sourced model goes through, and it caught
three real mesh defects the first time a real asset drop went through it. It
had no test.

That is a worse gap than it looks, because every one of its failure modes is
silent: a mis-parsed negative index scrambles a mesh that still converts, a
missed `usemtl` boundary merges two materials into one primitive, and a
double-sided pair handled wrongly loses a surface from one side only. None of
those fails a build; all of them cost an emulator capture and a guess.

Same discipline as test_prims.py / test_env.py / test_world.py: objkit imports
no bpy, so this runs under a bare python3 in milliseconds. Fixtures are
written to a temp directory rather than committed, because the point is to
exercise the parser against constructs the committed drop happens not to
contain (negative indices, quads, unnamed groups) as much as ones it does.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import objkit  # noqa: E402

FAIL = []


def check(cond, msg):
    if cond:
        print("  ok   %s" % msg)
    else:
        print("  FAIL %s" % msg)
        FAIL.append(msg)


def write(dirname, name, text):
    path = os.path.join(dirname, name)
    with open(path, "w") as fh:
        fh.write(text)
    return path


def normal(verts, face):
    """Unnormalised face normal, for winding comparisons."""
    a, b, c = (verts[i] for i in face)
    u = [b[i] - a[i] for i in range(3)]
    w = [c[i] - a[i] for i in range(3)]
    return (u[1] * w[2] - u[2] * w[1],
            u[2] * w[0] - u[0] * w[2],
            u[0] * w[1] - u[1] * w[0])


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def main():
    tmp = tempfile.mkdtemp(prefix="objkit-test-")

    # ── load_obj: indices ──────────────────────────────────────────────
    print("── load_obj: face indices ──")
    # The same triangle three ways: 1-based positive, negative (counting back
    # from the most recent vertex), and a quad that must fan-triangulate.
    # Every model in the drop is already triangles and uses positive indices,
    # which is exactly why the other two paths need a test — nothing else
    # exercises them, so a regression there would surface on the first
    # external .obj somebody imports and look like a corrupt file.
    path = write(tmp, "idx.obj", """
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
f 1 2 3
f -4 -3 -2
f 1 2 3 4
""")
    obj = objkit.load_obj(path)
    check(len(obj["verts"]) == 4, "4 vertices (%d)" % len(obj["verts"]))
    check(obj["faces"][0] == (0, 1, 2), "positive indices are 1-based")
    check(obj["faces"][1] == (0, 1, 2),
          "negative indices count back from the last vertex")
    check(obj["faces"][2:] == [(0, 1, 2), (0, 2, 3)],
          "a quad fan-triangulates into two tris, not dropped")
    check(all(max(f) < len(obj["verts"]) for f in obj["faces"]),
          "no face indexes past the vertex array")

    # ── load_obj: vertex colours ───────────────────────────────────────
    print("── load_obj: vertex colours ──")
    # The extended-OBJ convention: three EXTRA floats on the v line. The
    # distinction that matters is `colors is None` vs a list of white — a
    # caller that treats "no colours" as "white colours" renders a model that
    # should be lit as if it were pre-baked, which is exactly the difference
    # between a model meant to be shaded and one meant to be drawn unlit.
    plain = objkit.load_obj(write(tmp, "plain.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"))
    check(plain["colors"] is None, "no extended v lines -> colors is None")

    tinted = objkit.load_obj(write(tmp, "tint.obj", """
v 0 0 0 1.0 0.0 0.0
v 1 0 0 0.0 1.0 0.0
v 0 1 0 0.0 0.0 1.0
f 1 2 3
"""))
    check(tinted["colors"] is not None, "extended v lines -> colors present")
    check(tinted["colors"][1] == (0.0, 1.0, 0.0), "colour floats land in order")
    check(len(tinted["colors"]) == len(tinted["verts"]),
          "one colour per vertex (%d/%d)"
          % (len(tinted["colors"]), len(tinted["verts"])))

    # A file where only SOME vertices carry colour still reports colours, and
    # pads the rest to white — the alternative (returning None) would throw
    # away the ones that were authored.
    partial = objkit.load_obj(write(tmp, "partial.obj", """
v 0 0 0 0.5 0.5 0.5
v 1 0 0
v 0 1 0
f 1 2 3
"""))
    check(partial["colors"] is not None and len(partial["colors"]) == 3,
          "a partially-coloured file keeps the colours it has")
    check(partial["colors"][1] == (1.0, 1.0, 1.0),
          "uncoloured vertices pad to white")

    # ── load_mtl / group_faces ─────────────────────────────────────────
    print("── materials and groups ──")
    check(objkit.load_mtl(os.path.join(tmp, "nope.mtl")) == {},
          "a missing .mtl is {} rather than an exception")
    write(tmp, "two.mtl", "newmtl red\nKd 1.0 0.0 0.0\nnewmtl blue\nKd 0.0 0.0 1.0\n")
    mtl = objkit.load_mtl(os.path.join(tmp, "two.mtl"))
    check(mtl == {"red": (1.0, 0.0, 0.0), "blue": (0.0, 0.0, 1.0)},
          "load_mtl reads every newmtl/Kd pair")

    grouped = objkit.load_obj(write(tmp, "grouped.obj", """
v 0 0 0
v 1 0 0
v 0 1 0
v 0 0 1
g hull
usemtl red
f 1 2 3
usemtl blue
f 1 2 4
f 1 3 4
"""))
    by_mat = objkit.group_faces(grouped, by="material")
    check(sorted(by_mat) == ["blue", "red"],
          "usemtl boundaries split faces (%s)" % sorted(by_mat))
    check(len(by_mat["red"]) == 1 and len(by_mat["blue"]) == 2,
          "each material keeps its own faces (red=%d blue=%d)"
          % (len(by_mat["red"]), len(by_mat["blue"])))
    by_grp = objkit.group_faces(grouped, by="group")
    check(list(by_grp) == ["hull"], "g lines split by group (%s)" % list(by_grp))

    # An OBJ with no usemtl at all must produce ONE primitive called
    # "default", not a primitive keyed on None — gltf_to_t3d silently drops a
    # primitive whose material name is missing (kilnlib.make_material's note),
    # so "default" is what keeps the geometry.
    bare = objkit.group_faces(plain, by="material")
    check(list(bare) == ["default"],
          "faces with no usemtl land under 'default' (%s)" % list(bare))

    # ── loose_parts ────────────────────────────────────────────────────
    print("── loose_parts ──")
    # Two disjoint triangles plus a two-triangle strip. The strip is the
    # biggest component and must come out first, because a caller that names
    # parts part_00.. in order relies on index 0 being the important one.
    parts = objkit.loose_parts([(0, 1, 2), (3, 4, 5), (6, 7, 8), (6, 8, 9)])
    check(len(parts) == 3, "three connected components (%d)" % len(parts))
    check(len(parts[0]) == 2, "largest component first (%d faces)" % len(parts[0]))
    check(sorted(len(p) for p in parts) == [1, 1, 2],
          "components are 2+1+1 faces")
    check(sum(len(p) for p in parts) == 4, "no face is lost or duplicated")

    # ── split_double_sided ─────────────────────────────────────────────
    print("── split_double_sided ──")
    # A real defect found in a real drop: two faces on the same three
    # vertices with OPPOSITE winding. This is content, not a mistake —
    # dropping one would make the surface vanish from one side under
    # backface culling — so the repair must clone vertices and keep BOTH
    # windings. Asserting on the face tuple alone would not catch a repair
    # that quietly re-wound the clone, so this checks the geometric normals.
    ds = {
        "verts": [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
        "colors": None,
        "faces": [(0, 1, 2), (0, 2, 1)],
        "face_material": ["hull", "hull"],
        "face_group": ["hull", "hull"],
    }
    before = normal(ds["verts"], ds["faces"][1])
    cloned = objkit.split_double_sided(ds)
    check(cloned == 1, "one twin cloned (%d)" % cloned)
    check(len(ds["verts"]) == 6, "three vertices added (%d)" % len(ds["verts"]))
    check(len(set(map(frozenset, ds["faces"]))) == 2,
          "the two faces no longer share a vertex set")
    after = normal(ds["verts"], ds["faces"][1])
    check(dot(before, after) > 0,
          "the clone keeps its ORIGINAL winding (n.n=%+.1f)" % dot(before, after))
    check(dot(normal(ds["verts"], ds["faces"][0]), after) < 0,
          "the pair is still double-sided after the split")

    # Colours must ride along with the cloned vertices, or the second face of
    # a double-sided surface comes out untinted on a model that renders unlit.
    dsc = {
        "verts": [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
        "colors": [(0.1, 0.2, 0.3), (0.4, 0.5, 0.6), (0.7, 0.8, 0.9)],
        "faces": [(0, 1, 2), (0, 2, 1)],
        "face_material": ["hull", "hull"],
        "face_group": ["hull", "hull"],
    }
    objkit.split_double_sided(dsc)
    check(len(dsc["colors"]) == len(dsc["verts"]),
          "colours stay one-per-vertex after cloning (%d/%d)"
          % (len(dsc["colors"]), len(dsc["verts"])))
    check(dsc["colors"][3] == (0.1, 0.2, 0.3),
          "cloned vertices carry their source colour")

    # A mesh with no twins must come out byte-identical, so the repair is safe
    # to run unconditionally on every model in the MODELS table.
    clean = {"verts": [(0.0, 0.0, 0.0)] * 4, "colors": None,
             "faces": [(0, 1, 2), (0, 2, 3)],
             "face_material": [None, None], "face_group": [None, None]}
    check(objkit.split_double_sided(clean) == 0 and len(clean["verts"]) == 4,
          "a mesh with no twins is left alone")

    # ── bbox / yup_to_zup ──────────────────────────────────────────────
    print("── bbox and axis conversion ──")
    # Componentwise, NOT "the vertex with the smallest sum" — the two differ
    # for any mesh whose extremes are on different vertices, which is every
    # real mesh. Mixed signs per axis on purpose so a transposed or
    # sign-flipped implementation cannot pass by coincidence.
    lo, hi = objkit.bbox([(-1.0, 2.0, -3.0), (4.0, -5.0, 6.0)])
    check(lo == [-1.0, -5.0, -3.0] and hi == [4.0, 2.0, 6.0],
          "bbox is componentwise min/max (lo=%s hi=%s)" % (lo, hi))

    # The conversion must be a proper ROTATION, determinant +1. objkit's own
    # docstring records what the reflection (x, z, y) costs: a model's head
    # pointing backwards out of its own neck (n64-animation skill's failure-
    # mode table). A reflection flips handedness and inverts every normal,
    # and the model still builds — so this is checked arithmetically rather
    # than trusted.
    cols = [objkit.yup_to_zup(b) for b in ((1, 0, 0), (0, 1, 0), (0, 0, 1))]
    det = (cols[0][0] * (cols[1][1] * cols[2][2] - cols[1][2] * cols[2][1])
           - cols[1][0] * (cols[0][1] * cols[2][2] - cols[0][2] * cols[2][1])
           + cols[2][0] * (cols[0][1] * cols[1][2] - cols[0][2] * cols[1][1]))
    check(abs(det - 1.0) < 1e-9,
          "yup_to_zup is a rotation, not a reflection (det=%+.1f)" % det)
    check(objkit.yup_to_zup((1.0, 2.0, 3.0)) == (1.0, -3.0, 2.0),
          "yup_to_zup maps (x,y,z) -> (x,-z,y)")

    print()
    if FAIL:
        print("%d check(s) FAILED" % len(FAIL))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
