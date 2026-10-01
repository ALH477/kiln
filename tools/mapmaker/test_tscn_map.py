#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""test_tscn_map.py — the Redot level path, checked without Redot.

    python3 tools/mapmaker/test_tscn_map.py --assets assets

bpy-free and Redot-free on purpose: a `.tscn` is text, so the whole converter
is testable from a bare python3 and nix/checks/tscn-map.nix can gate it with
no editor in the closure. Same shape as tools/blender/test_quake_map.py -- a
module-level FAIL list, a check(), and main() returning 1.

── What this is really guarding ────────────────────────────────────────────
Two silent failures, both of which have already happened in this tree:

  * A WRONG AXIS OR YAW CONVENTION. A level that is mirrored or turned
    ninety degrees loads, collides and plays; it is only wrong against an
    intent nothing else records. So the conventions are proven against a
    fixture AND the plausible wrong readings are asserted to be rejected,
    exactly as tools/blender/demonrig.py's verify_convention does.

  * AN INSIDE-OUT BRUSH. It loads on console, collides correctly, and draws
    NOTHING. assets/pm_lab.map in PetaByte-Madness shipped that way -- all 15
    brushes, 0 CSG faces -- because tools/gen_lab_map.py hand-rolled its own
    winding table instead of using the schema's. Every brush this converter
    emits is therefore run through the real CSG here and required to come
    back whole.
"""

import argparse
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "blender"))
sys.path.insert(0, str(HERE.parent / "schema"))

import mapfmt          # noqa: E402
import quake_map       # noqa: E402
import tscn_map        # noqa: E402
import level_vocab     # noqa: E402

FAIL = []


def check(cond, msg):
    print("  %s %s" % ("ok  " if cond else "FAIL", msg))
    if not cond:
        FAIL.append(msg)


def scene(body, root='[node name="Level" type="Node3D"]\n'):
    return "[gd_scene load_steps=1 format=3]\n\n" + root + "\n" + body


def box_node(name, origin=(0, 0, 0), size=(1, 1, 1), tex=None, parent=".",
             basis=(1, 0, 0, 0, 1, 0, 0, 0, 1)):
    s = ('[node name="%s" type="CSGBox3D" parent="%s"]\n'
         'transform = Transform3D(%s, %s, %s)\n'
         'size = Vector3(%g, %g, %g)\n'
         % (name, parent, ", ".join("%g" % c for c in basis),
            ", ".join("%g" % c for c in origin[:0] or ()) or
            ", ".join("%g" % c for c in origin), "", size[0], size[1], size[2]))
    # Rebuild cleanly: Transform3D is 9 basis floats then 3 origin floats.
    s = ('[node name="%s" type="CSGBox3D" parent="%s"]\n'
         'transform = Transform3D(%s, %s)\n'
         'size = Vector3(%g, %g, %g)\n'
         % (name, parent,
            ", ".join("%g" % c for c in basis),
            ", ".join("%g" % c for c in origin),
            size[0], size[1], size[2]))
    if tex:
        s += 'metadata/kiln_texture = "%s"\n' % tex
    return s


def marker(name, origin=(0, 0, 0), yaw_deg=0.0, classname="info_player_start",
           epairs=None, parent="."):
    th = math.radians(yaw_deg)
    c, s_ = math.cos(th), math.sin(th)
    basis = (c, 0, -s_, 0, 1, 0, s_, 0, c)
    out = ('[node name="%s" type="Marker3D" parent="%s"]\n'
           'transform = Transform3D(%s, %s)\n'
           'metadata/kiln_classname = "%s"\n'
           % (name, parent, ", ".join("%g" % x for x in basis),
              ", ".join("%g" % x for x in origin), classname))
    for k, v in (epairs or {}).items():
        out += 'metadata/kiln_epair_%s = "%s"\n' % (k, v)
    return out


# ── A. the conventions, proven and their alternatives refused ───────────────

def test_conventions():
    print("A  axes, scale and yaw")

    # One box: centred at (1, 2, 3) metres, 4 x 2 x 8 metres, scale 64.
    st = tscn_map.scene_to_state(scene(box_node("B", (1, 2, 3), (4, 2, 8))))
    b = st["brushes"][0]
    # centre (64,128,192) units, half-extents (128,64,256).
    check(b == {"mins": [-64, 64, -64], "maxs": [192, 192, 448],
                "texture": "TEX"},
          "a 4x2x8 m box centred at (1,2,3) m is mins(-64,64,-64) "
          "maxs(192,192,448) at 64 units/m -- got %s" % b)

    # The wrong readings this could plausibly have been written with. Each is
    # a level that loads, collides and plays, and is simply in the wrong place.
    check(b["mins"][2] != -448,
          "Z is NOT negated (Godot's -Z 'forward' is a camera convention, "
          "not a different basis)")
    check((b["mins"][1], b["mins"][2]) != (-64, 64),
          "Y and Z are NOT swapped (that is the BLENDER conversion, and this "
          "is not Blender)")
    check(b["mins"] != [64, 128, 192],
          "a box primitive is CENTRED on its node; `size` is not 0..size")
    check(b["maxs"][0] - b["mins"][0] == 256,
          "the scale is 64 units to the metre, so a 4 m box is 256 units wide")

    # Yaw. The engine's forward is (sin yaw, 0, cos yaw) and a Godot node
    # turned by theta about Y has its +Z column at (sin theta, 0, cos theta).
    for deg in (0, 45, 90, 180, 270):
        st = tscn_map.scene_to_state(scene(marker("M", (0, 0, 0), deg)))
        got = st["spawns"][0]["angle"]
        check(got == deg, "a node turned %d degrees about Y is angle %d "
                          "(got %d)" % (deg, deg, got))

    st = tscn_map.scene_to_state(scene(marker("M", (0, 0, 0), 90)))
    check(st["spawns"][0]["angle"] != 270,
          "yaw is NOT mirrored (90 would read as 270)")
    check(st["spawns"][0]["angle"] != 0,
          "yaw is read from the basis, not dropped")

    # Origin, at scale.
    st = tscn_map.scene_to_state(scene(marker("M", (0.5, 1.0, -2.0), 0)))
    check(st["spawns"][0]["origin"] == [32, 64, -128],
          "a spawn origin scales by 64 and is otherwise identity -- got %s"
          % st["spawns"][0]["origin"])


# ── B. the Vector3 digit trap ───────────────────────────────────────────────

def test_value_syntax():
    print("B  Godot's value syntax")
    check(tscn_map.vector3("Vector3(5, 0.3125, 15.625)") == (5.0, 0.3125, 15.625),
          "Vector3's own '3' is not read as a component (it was, and every "
          "box came out with all three extents shifted by one place)")
    check(tscn_map.vector3("Vector3(-1, -2, -3)") == (-1.0, -2.0, -3.0),
          "negative components survive")
    pts = tscn_map.packed_vector3_array("PackedVector3Array(0, 0, 0, 1, 2, 3)")
    check(pts == [(0.0, 0.0, 0.0), (1.0, 2.0, 3.0)],
          "PackedVector3Array's own '3' is not read as a component")
    check(tscn_map.sub_resource_id('SubResource("BoxMesh_1")') == "BoxMesh_1",
          "SubResource id is read")


# ── C. box vs convex, and parent composition ────────────────────────────────

def test_shapes():
    print("C  brush shape and node parenting")

    # 90 degrees about Y is still an axis-aligned box.
    st = tscn_map.scene_to_state(scene(
        box_node("B", (0, 0, 0), (2, 1, 4), basis=(0, 0, -1, 0, 1, 0, 1, 0, 0))))
    check("mins" in st["brushes"][0],
          "a 90-degree rotation stays the BOX form (it is still axis-aligned)")
    check(st["brushes"][0]["mins"] == [-128, -32, -64],
          "and its extents swap accordingly -- got %s" % st["brushes"][0]["mins"])

    # 30 degrees is not.
    c, s_ = math.cos(math.radians(30)), math.sin(math.radians(30))
    st = tscn_map.scene_to_state(scene(
        box_node("B", (0, 0, 0), (2, 1, 4), basis=(c, 0, -s_, 0, 1, 0, s_, 0, c))))
    check("convex" in st["brushes"][0],
          "a 30-degree rotation becomes the CONVEX form")
    check(len(st["brushes"][0]["convex"]) == 8,
          "and keeps all 8 corners -- got %d"
          % len(st["brushes"][0]["convex"]))

    # A brush under a translated parent is where the PARENT puts it.
    body = ('[node name="Group" type="Node3D" parent="."]\n'
            'transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 10, 0, 0)\n\n'
            + box_node("B", (1, 0, 0), (1, 1, 1), parent="Group"))
    st = tscn_map.scene_to_state(scene(body))
    check(st["brushes"][0]["mins"][0] == 672,
          "a brush under a translated parent composes the chain (11 m -> 704 "
          "centre, 672 min) -- got %s" % st["brushes"][0]["mins"][0])

    # ConvexPolygonShape3D: a wedge.
    pts = [(0, 0, 0), (2, 0, 0), (0, 0, 1), (2, 0, 1), (0, 1, 1), (2, 1, 1)]
    body = ('[sub_resource type="ConvexPolygonShape3D" id="S1"]\n'
            'points = PackedVector3Array(%s)\n\n'
            % ", ".join("%g" % c for p in pts for c in p)
            + '[node name="Ramp" type="CollisionShape3D" parent="."]\n'
              'transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)\n'
              'shape = SubResource("S1")\n')
    st = tscn_map.scene_to_state(scene(body))
    check(len(st["brushes"]) == 1 and "convex" in st["brushes"][0],
          "a ConvexPolygonShape3D becomes a convex brush")


# ── D. entities ─────────────────────────────────────────────────────────────

def test_entities():
    print("D  spawns and epairs")
    st = tscn_map.scene_to_state(scene(
        marker("Imp", (1, 0, 2), 180, "info_imp", {"count": "3"})))
    s = st["spawns"][0]
    check(s["classname"] == "info_imp", "classname comes from metadata")
    check(s["epairs"] == {"count": "3"}, "epairs come from metadata -- got %s"
          % s["epairs"])
    check(not st["brushes"],
          "a node with a classname is a spawn, not also a brush")

    body = marker("A", classname="info_player_start")
    root = ('[node name="Level" type="Node3D"]\n'
            'metadata/kiln_worldspawn_sky = "night"\n')
    st = tscn_map.scene_to_state(scene(body, root=root))
    check(st.get("worldspawn") == {"sky": "night"},
          "worldspawn epairs come off the root -- got %s" % st.get("worldspawn"))


# ── E. the limits, refused where the author can still see them ──────────────

def test_limits():
    print("E  engine limits")
    lim = level_vocab.limits()

    # Spaced so that 256 of them stay well inside the COORD limit -- spread
    # them 3 m apart and the coord check fires first, which tests the wrong
    # thing.
    many = "\n".join(box_node("B%d" % i, (i * 0.1, 0, 0), (1, 1, 1))
                     for i in range(lim["brushes"] + 1))
    st = tscn_map.scene_to_state(scene(many))
    check(bool(tscn_map.check_limits(st)),
          "%d brushes is refused (kiln_map.c loads %d and DROPS the rest)"
          % (lim["brushes"] + 1, lim["brushes"]))

    ok = "\n".join(box_node("B%d" % i, (i * 0.1, 0, 0), (1, 1, 1))
                   for i in range(lim["brushes"]))
    check(not tscn_map.check_limits(tscn_map.scene_to_state(scene(ok))),
          "exactly %d brushes is accepted" % lim["brushes"])

    far = lim["coord"] / 64.0 + 10
    st = tscn_map.scene_to_state(scene(box_node("B", (far, 0, 0), (1, 1, 1))))
    check(bool(tscn_map.check_limits(st)),
          "a brush past +/-%d units is refused (face vertices are int16)"
          % lim["coord"])

    spawns = "\n".join(marker("S%d" % i, (i, 0, 0))
                       for i in range(lim["spawns"] + 1))
    check(bool(tscn_map.check_limits(tscn_map.scene_to_state(scene(spawns)))),
          "%d spawns is refused" % (lim["spawns"] + 1))


# ── F. nothing this converter emits is ever inside-out ──────────────────────

def emitted_is_whole(text, label):
    ents = quake_map.parse_map(text)
    bad = []
    for i, brush in enumerate(ents[0]["brushes"]):
        n = len([p for ps in quake_map.brush_to_faces(brush).values() for p in ps])
        if n < len(brush):
            bad.append("#%d got %d of %d" % (i + 1, n, len(brush)))
    check(not bad, "%s: every brush survives the CSG whole%s"
          % (label, "" if not bad else " -- " + ", ".join(bad[:3])))


def test_refuses_to_emit_broken():
    """The guard that ACTUALLY caught the inside-out defects, asserted.

    Seeding an inverted winding into this converter and watching the suite go
    red proved something slightly different from what test F below claims:
    `to_map` self-validates and raises, so it aborts before F's own CSG loop
    ever runs. F was taking credit for a catch the emitter had already made,
    and as written it could not fail for an emit-path defect at all.

    So prove the guard itself. Flip the canonical winding under the emitter --
    the exact inversion tools/gen_lab_map.py shipped -- and require a refusal.
    If this ever stops raising, a level that draws nothing can reach a ROM
    again."""
    print("F0 the emitter REFUSES to write a level that does not validate")
    good = scene(box_node("Floor", (0, -0.5, 0), (10, 1, 20), tex="DECK")
                 + marker("Start", (0, 0.5, -8), 0))

    real = mapfmt.aabb_faces
    try:
        # p2 and p3 swapped on every face: loads on console, collides
        # correctly, draws nothing.
        mapfmt.aabb_faces = lambda mn, mx: [(f[0], f[2], f[1]) for f in real(mn, mx)]
        try:
            tscn_map.to_map(good, 64.0)
            check(False, "an inside-out winding is refused, not written")
        except SystemExit as e:
            check("does not validate" in str(e),
                  "an inside-out winding is refused, and says so: %s"
                  % str(e).split(";")[0])
    finally:
        mapfmt.aabb_faces = real

    # And the guard is not simply always-on: the same scene, unflipped, passes.
    _text, _state, _warn, report = tscn_map.to_map(good, 64.0)
    check(not report["problems"],
          "and the same scene with the real winding emits cleanly")


def test_never_inside_out():
    print("F  emitted brushes survive the CSG (the pm_lab.map failure)")
    c, s_ = math.cos(math.radians(37)), math.sin(math.radians(37))
    body = (box_node("Floor", (0, -0.5, 0), (10, 1, 20), tex="DECK")
            + box_node("Turn", (3, 1, 2), (2, 2, 2), tex="STONE",
                       basis=(c, 0, -s_, 0, 1, 0, s_, 0, c))
            + marker("Start", (0, 0.5, -8), 0))
    text, state, _warn, report = tscn_map.to_map(scene(body), 64.0)
    emitted_is_whole(text, "a scene with a rotated brush")
    check(not report["problems"],
          "and map-validate finds no problems -- got %s" % report["problems"])
    check(len(report["aabb_only"]) == 1,
          "the rotated brush is WARNED about as collides-as-AABB (got %d)"
          % len(report["aabb_only"]))


# ── G. round-trip accuracy against the committed levels ─────────────────────

def test_roundtrip(assets):
    print("G  .map -> .tscn -> .map preserves the level exactly")
    maps = sorted(Path(assets).glob("*.map"))
    check(bool(maps), "found .map fixtures under %s" % assets)
    for m in maps:
        before = mapfmt.to_state(quake_map.parse_map(m.read_text()))
        tscn = tscn_map.state_to_scene(before)
        after = tscn_map.scene_to_state(tscn)
        check(before["brushes"] == after["brushes"],
              "%s: %d brushes survive the round trip identically"
              % (m.name, len(before["brushes"])))
        check([(s["classname"], s["origin"], s["angle"]) for s in before["spawns"]]
              == [(s["classname"], s["origin"], s["angle"]) for s in after["spawns"]],
              "%s: %d spawns survive with their origin and angle"
              % (m.name, len(before["spawns"])))
        # And the .map it produces is stable, and whole.
        t1 = mapfmt.emit_state(after)
        t2 = mapfmt.emit_state(mapfmt.to_state(quake_map.parse_map(t1)))
        check(t1 == t2, "%s: the emitted .map is byte-stable" % m.name)
        emitted_is_whole(t1, m.name)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", default="assets")
    a = ap.parse_args(argv)

    test_conventions()
    test_value_syntax()
    test_shapes()
    test_entities()
    test_limits()
    test_refuses_to_emit_broken()
    test_never_inside_out()
    test_roundtrip(a.assets)

    print()
    if FAIL:
        print("FAILED %d check(s):" % len(FAIL), file=sys.stderr)
        for f in FAIL:
            print("  - " + f, file=sys.stderr)
        return 1
    print("tscn_map: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
