#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""tscn_map.py — a Godot/Redot scene is the level; this turns it into `.map`.

    ./dev map-from-tscn level.tscn out.map      # author in Redot, build a ROM
    ./dev map-to-tscn   level.map  out.tscn     # open an existing level again

── Why a converter and not a Godot exporter addon ──────────────────────────
tools/mapmaker/README.md says, in bold, do not hand-write `.map` text: an
inside-out brush loads on console, collides correctly and draws NOTHING, and
six of this repo's seven committed `.map` files were wound inward before
anything noticed. tools/schema/level_vocab.py's docstring records the same
lesson for the vocabulary -- the classname, epair, limit and winding tables
once lived in six hand-maintained copies that had drifted.

A GDScript emitter inside the editor would be the seventh copy, in the one
language no check in this tree runs. So the editor writes a `.tscn` -- which it
already does, for free, as its own save format -- and all the format knowledge
stays here, next to mapfmt.py, behind the same gates. tools/redot/'s addon is a
dock that shells out to this file and reads level_vocab.json for its limits; it
holds no winding and no numbers of its own.

It also means every gate runs with no Redot installed, which matters, because
Redot is not in nixpkgs.

── Axes: there is no conversion, and that is the whole point ────────────────
Godot's Transform3D and this engine are the same basis -- right-handed, +Y up.
kiln_map.c applies no axis swap, no negation and no scale (`origin` is
sscanf'd into spawn->pos verbatim, kiln_dict.c's fig_dict_parse_vec3), so every
`.map` this engine loads is ALREADY authored in Godot's convention. Positions
are therefore identity, times the unit scale below.

Yaw is identity too, which is worth stating because it looks like it should not
be. kiln_fpscam's yaw is 0 at +Z with forward = (sin yaw, 0, cos yaw)
(PetaByte-Madness/src/pm_lab.c). A Godot node rotated by theta about Y has its
+Z basis column at (sin theta, 0, cos theta) -- the same vector. So:

    engine `angle` in degrees == the node's Godot Y rotation in degrees
    and a spawn faces along its own +Z axis

"Forward is -Z" is Godot's convention for where a CAMERA looks, not a property
of the coordinate system, and it does not enter here. tools/mapmaker/
test_tscn_map.py proves both of these against a known-good fixture and asserts
the plausible wrong conventions are REJECTED, the way
tools/blender/demonrig.py's verify_convention does -- because a silently
mirrored level is indistinguishable from a level someone built mirrored.

── Bit-for-bit across architectures ────────────────────────────────────────
The same scene must produce a byte-identical `.map` on every machine, so this
file restricts itself to arithmetic IEEE-754 pins down:

  * `+ - * /` are correctly rounded by the standard, and `sqrt` is too, so the
    hull solver in mapfmt.py (dot, cross, normalise, compare) is exact and
    reproducible. Its plane ORDER is sorted and its point order is sorted, so
    no dict or set iteration order reaches the file.
  * libm's TRANSCENDENTALS are not correctly rounded and differ between glibc,
    musl and versions of each. `atan2` and `acos` are therefore kept off the
    output path: an axis-aligned basis -- which is all of 0/90/180/270, and so
    nearly every spawn anyone authors -- is read by exact comparison, and the
    general case is rounded with a tolerance far wider than any libm
    disagreement before it becomes the integer the file stores.
  * Formatting is `%.12g` and Python's float repr, both correctly rounded
    decimal conversions, and -0.0 is normalised to 0.0.

nix/checks/tscn-map.nix is the actual proof: it regenerates the committed
reference `.map` and `.tscn` and diffs them, the same regenerate-and-diff
shape nix/checks/level-vocab.nix and pm-gen-headers.nix use. Run it on a
second architecture and a disagreement is a build failure, not a surprise in
somebody's level.

── Units ───────────────────────────────────────────────────────────────────
Author in Redot at its native 1 unit = 1 metre. This engine runs at 64 units to
the metre (docs/ASSET_PIPELINE.md's Conventions, and gltf_to_t3d
--base-scale=64), so `--scale` defaults to 64 and one Redot metre is one game
metre. A 0.125 m grid snap lands every coordinate on a whole unit (8 of them).

── The authoring vocabulary ────────────────────────────────────────────────
Deliberately small, and all of it plain Godot nodes with no script attached, so
a scene opens in a stock editor and the addon is a convenience rather than a
requirement.

  CSGBox3D                      a brush. `size` is its full extent, centred.
  MeshInstance3D + BoxMesh      a brush, for anyone who prefers a mesh.
  *Shape3D + ConvexPolygonShape3D   a brush of arbitrary convex shape.
  metadata/kiln_texture         that brush's surface name (default TEX).
  metadata/kiln_classname       makes ANY node a spawn instead.
  metadata/kiln_epair_<key>     one epair on that spawn.
  metadata/kiln_worldspawn_<k>  a worldspawn epair, from any node.

A brush that comes out axis-aligned is emitted as mins/maxs; anything else is
emitted as a convex hull. That is not a style choice -- see mapfmt.py's convex
section: kiln_clip collides EVERY brush as its bounding box, so a rotated or
wedge brush draws its true shape and blocks a box. Keep anything walkable
axis-aligned. `./dev map-validate` warns, per brush, when one is not.
"""

import argparse
import json
import math
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "blender"))
sys.path.insert(0, str(HERE.parent / "schema"))

import godot_scene          # noqa: E402
import level_vocab          # noqa: E402
import mapfmt               # noqa: E402
import quake_map            # noqa: E402

LIMITS = level_vocab.limits()

META_TEX = "metadata/kiln_texture"
META_CLASS = "metadata/kiln_classname"
META_EPAIR = "metadata/kiln_epair_"
META_WORLD = "metadata/kiln_worldspawn_"

BRUSH_MESH_TYPES = ("BoxMesh",)
BRUSH_SHAPE_TYPES = ("ConvexPolygonShape3D", "BoxShape3D")

# A node whose world basis is a signed permutation within this tolerance is
# treated as axis-aligned. Godot writes a 90-degree rotation as 1/-1 and a
# float 4.37e-08, not as an exact zero.
AXIS_EPS = 1e-4


def die(msg):
    raise SystemExit("tscn_map: " + msg)


def _coord(v):
    """A world coordinate, cleaned of float noise but NOT rounded to a unit.

    A Transform3D composed through a parent chain lands on 63.999999997 where
    it means 64, and emitting that is unreadable; but assets/hangar.map really
    does carry 0.4, and rounding a box to whole units silently moved a lip the
    author put there. So: snap the noise at 4 decimals, then mapfmt's own
    int-where-integral rule.

    Convex brushes are the deliberate exception and DO snap to whole units --
    see hull_planes: their planes are re-derived from three stored points, so
    the points have to be exact for the file to be a fixed point. A box's
    mins/maxs are stored as themselves and need none of that."""
    return mapfmt._num_val(round(float(v), 4))


# ── reading Godot's value syntax ────────────────────────────────────────────

def _inside_parens(raw):
    """The text between the first '(' and the last ')'.

    Godot's value syntax puts a DIGIT IN THE TYPE NAME -- Vector3,
    Transform3D, PackedVector3Array -- so a bare number scan over the whole
    expression picks up that 3 as the first component and shifts every real
    one along by a place. `Vector3(5, 0.3125, 15.625)` read as (3, 5, 0.3125)
    is a box with the wrong extent on all three axes, which is how a whole
    level came back distorted rather than failing."""
    raw = raw or ""
    i, j = raw.find("("), raw.rfind(")")
    return raw[i + 1:j] if 0 <= i < j else raw


def _floats(raw):
    return [float(x) for x in re.findall(r"-?\d+\.?\d*(?:e[-+]?\d+)?",
                                         _inside_parens(raw), re.I)]


def vector3(raw, default=(0.0, 0.0, 0.0)):
    """`Vector3(4, 2, 8)` -> (4.0, 2.0, 8.0)."""
    if not raw:
        return tuple(default)
    v = _floats(raw)
    if len(v) < 3:
        return tuple(default)
    return (v[0], v[1], v[2])


def packed_vector3_array(raw):
    """`PackedVector3Array(0, 0, 0, 1, 0, 0, ...)` -> [(x,y,z), ...]."""
    v = _floats(raw)
    if len(v) % 3:
        die("a PackedVector3Array has %d numbers, which is not a whole number "
            "of points" % len(v))
    return [tuple(v[i:i + 3]) for i in range(0, len(v), 3)]


def sub_resource_id(raw):
    """`SubResource("BoxMesh_1")` -> "BoxMesh_1", else None."""
    m = re.match(r'SubResource\(\s*"?([^")]+)"?\s*\)', (raw or "").strip())
    return m.group(1) if m else None


def unquote(raw):
    raw = (raw or "").strip()
    if len(raw) >= 2 and raw[0] == '"' and raw[-1] == '"':
        return raw[1:-1]
    return raw


# ── transforms ──────────────────────────────────────────────────────────────

IDENTITY = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0)


def xform_of(node):
    t = node.get("transform")
    return tuple(t) if t else IDENTITY


def xform_mul(a, b):
    """Compose two Godot Transform3Ds, `a` outer. Stored as three basis
    COLUMNS then the origin, which is the order .tscn writes them."""
    ax, ay, az, ao = a[0:3], a[3:6], a[6:9], a[9:12]

    def apply_basis(v):
        return tuple(ax[i] * v[0] + ay[i] * v[1] + az[i] * v[2] for i in range(3))

    bx, by, bz, bo = b[0:3], b[3:6], b[6:9], b[9:12]
    cx, cy, cz = apply_basis(bx), apply_basis(by), apply_basis(bz)
    co = tuple(apply_basis(bo)[i] + ao[i] for i in range(3))
    return cx + cy + cz + co


def xform_point(t, p):
    bx, by, bz, o = t[0:3], t[3:6], t[6:9], t[9:12]
    return tuple(bx[i] * p[0] + by[i] * p[1] + bz[i] * p[2] + o[i]
                 for i in range(3))


def node_paths(scene):
    """{node path -> node}, and each node's composed WORLD transform.

    .tscn stores `parent` as a path relative to the scene root ("." for a
    root child, "Walls/Inner" deeper), and a transform relative to that
    parent -- so a brush grouped under a rotated Node3D is not where its own
    transform says it is. Composing the chain is the difference between
    honouring the editor's grouping and quietly ignoring it."""
    by_path = {}
    order = []
    for n in scene["nodes"]:
        parent = n.get("parent")
        if parent is None:
            path = "."                      # the scene root itself
        elif parent == ".":
            path = n["name"]
        else:
            path = parent + "/" + n["name"]
        if path in by_path:
            die("two nodes share the path %r; rename one" % path)
        by_path[path] = n
        order.append((path, n))

    world = {}

    def resolve(path, node):
        if path in world:
            return world[path]
        parent = node.get("parent")
        if parent is None:
            world[path] = xform_of(node)
        else:
            pnode = by_path.get(parent)
            if pnode is None:
                # A parent the file never declared. Treat the node as rooted
                # rather than dropping it: losing a brush silently is worse
                # than placing it where its own transform says.
                world[path] = xform_of(node)
            else:
                world[path] = xform_mul(resolve(parent, pnode), xform_of(node))
        return world[path]

    for path, node in order:
        resolve(path, node)
    return by_path, world, order


# ── brush geometry ──────────────────────────────────────────────────────────

def box_corners(size):
    """A Godot box primitive is CENTRED on its node and `size` is the full
    extent, so the corners are at +/- size/2 -- not 0..size."""
    hx, hy, hz = size[0] / 2.0, size[1] / 2.0, size[2] / 2.0
    return [(x, y, z) for x in (-hx, hx) for y in (-hy, hy) for z in (-hz, hz)]


def axis_aligned_box(pts, eps=AXIS_EPS):
    """(mins, maxs) when these points are exactly the 8 corners of an
    axis-aligned box, else None.

    Tested on the RESULT rather than on the node's basis, so a 90-degree
    rotation, a negative scale and a plain translation all land on the box
    form by themselves, with no special case per node type."""
    if len(pts) != 8:
        return None
    mn = [min(p[i] for p in pts) for i in range(3)]
    mx = [max(p[i] for p in pts) for i in range(3)]
    for i in range(3):
        if mx[i] - mn[i] <= eps:
            return None                     # flat: no volume
    for p in pts:
        for i in range(3):
            if abs(p[i] - mn[i]) > eps and abs(p[i] - mx[i]) > eps:
                return None                 # a corner off the box
    return mn, mx


def brush_from_node(node, scene, world, scale):
    """The brush a node describes, or None if it describes none."""
    props = node.get("props", {})
    ntype = node.get("type") or ""
    local = None

    if ntype == "CSGBox3D":
        # Godot's own default when `size` is left alone.
        local = box_corners(vector3(props.get("size"), (2.0, 2.0, 2.0)))
    else:
        sub = scene.get("sub_resources", {})
        mesh = sub.get(sub_resource_id(props.get("mesh")))
        if mesh and mesh["type"] in BRUSH_MESH_TYPES:
            local = box_corners(vector3(mesh["props"].get("size"),
                                        (2.0, 2.0, 2.0)))
        else:
            shape = sub.get(sub_resource_id(props.get("shape")))
            if shape and shape["type"] == "ConvexPolygonShape3D":
                local = packed_vector3_array(shape["props"].get("points"))
                if len(local) < 4:
                    die("%s's ConvexPolygonShape3D has %d points; 4+ are "
                        "needed to enclose a volume"
                        % (node["name"], len(local)))
            elif shape and shape["type"] == "BoxShape3D":
                local = box_corners(vector3(shape["props"].get("size"),
                                            (2.0, 2.0, 2.0)))
    if local is None:
        return None

    pts = [tuple(c * scale for c in xform_point(world, p)) for p in local]
    tex = unquote(props.get(META_TEX)) or "TEX"
    box = axis_aligned_box(pts)
    if box is not None:
        return {"mins": [_coord(v) for v in box[0]],
                "maxs": [_coord(v) for v in box[1]], "texture": tex}
    return {"convex": [[round(c) for c in p] for p in pts], "texture": tex}


# ── yaw ─────────────────────────────────────────────────────────────────────

# How close a basis column has to be to an axis to be treated as exactly on
# it. Godot writes a quarter turn as 1, -1 and a float 4.37e-08.
AXIS_SNAP = 1e-6

# cos(1 degree), as a literal rather than a call: see the docstring's note on
# transcendentals. A spawn leaning further than this off upright is reported.
COS_ONE_DEGREE = 0.9998476951563913


def yaw_degrees(world):
    """The node's Y rotation in degrees -- which IS the engine's `angle`.

    Read off the +Z basis column projected into XZ, because that column is the
    direction the spawn faces and the engine's forward is (sin yaw, 0, cos yaw)
    for the same yaw. See the module docstring.

    The four axis-aligned cases are answered by comparison and never reach
    atan2. That is not an optimisation: atan2 is not correctly rounded and
    differs between libm implementations, so a level authored at a quarter turn
    could otherwise emit 90 on one machine and 89 on another. Everything a
    designer actually builds -- walls, doors, a spawn facing down a corridor --
    lands here."""
    x, z = world[6], world[8]
    n = math.sqrt(x * x + z * z)          # sqrt IS correctly rounded
    if n < 1e-12:
        return 0.0                        # degenerate: no facing to read
    x, z = x / n, z / n
    for deg, (sx, sz) in ((0, (0.0, 1.0)), (90, (1.0, 0.0)),
                          (180, (0.0, -1.0)), (270, (-1.0, 0.0))):
        if abs(x - sx) < AXIS_SNAP and abs(z - sz) < AXIS_SNAP:
            return float(deg)
    # Normalised to [0, 360). atan2 returns negatives, and assets/pm_lab.map
    # writes "270" where atan2 says -90 -- the engine reads either, but a
    # round trip that rewrites every angle is a diff nobody can review.
    return math.degrees(math.atan2(x, z)) % 360.0


def yaw_angle(world):
    """`yaw_degrees` as the integer the file stores.

    Rounded through a nudge so that a value sitting on a .5 boundary cannot be
    decided by the last bit of atan2. libm implementations disagree in the
    region of 1e-13 degrees; 1e-6 is seven orders clear of that and still far
    below a degree, so this changes no angle anyone authored."""
    d = yaw_degrees(world)
    n = math.floor(d + 0.5 + 1e-6)
    return int(n % 360)


def is_tilted(world):
    """Does this node lean more than a degree off world up?

    The engine stores a single `angle` and nothing else, so pitch and roll are
    DROPPED -- worth reporting rather than silently flattening a tilted spawn.
    Compared against the cosine directly: acos is a transcendental, and a
    yes/no answer does not need one."""
    by = world[3:6]
    n = math.sqrt(sum(c * c for c in by))
    if n < 1e-12:
        return False
    return (by[1] / n) < COS_ONE_DEGREE


# ── .tscn -> state ──────────────────────────────────────────────────────────

def scene_to_state(text, scale=64.0, warn=None):
    """A parsed .tscn -> the state shape mapfmt.emit_state consumes."""
    warn = warn if warn is not None else []
    scene = godot_scene.parse_tscn(text)
    _by_path, world, order = node_paths(scene)

    brushes, spawns, worldspawn = [], [], {}

    for path, node in order:
        props = node.get("props", {})
        w = world[path]

        for k, raw in props.items():
            if k.startswith(META_WORLD):
                worldspawn.setdefault(k[len(META_WORLD):], unquote(raw))

        classname = unquote(props.get(META_CLASS)) if META_CLASS in props else None
        if classname:
            epairs = {k[len(META_EPAIR):]: unquote(v)
                      for k, v in props.items() if k.startswith(META_EPAIR)}
            # fig_dict caps an entity at this many keys INCLUDING the three
            # below, and silently drops the rest on console.
            if len(epairs) + 3 > 16:
                die("%s carries %d epairs; a FigDict holds 16 keys including "
                    "classname, origin and angle" % (node["name"], len(epairs)))
            if is_tilted(w):
                warn.append("%s leans off upright; the engine stores only a "
                            "yaw, so its pitch and roll are dropped"
                            % node["name"])
            spawns.append({
                "classname": classname,
                "origin": [_coord(c * scale) for c in w[9:12]],
                "angle": yaw_angle(w),
                "epairs": epairs,
            })
            continue

        brush = brush_from_node(node, scene, w, scale)
        if brush is not None:
            brushes.append(brush)

    state = {"brushes": brushes, "spawns": spawns}
    if worldspawn:
        state["worldspawn"] = worldspawn
    return state


# ── limits ──────────────────────────────────────────────────────────────────

def check_limits(state):
    """Every ceiling kiln_map.c enforces by DROPPING things, checked where the
    author can still see it. A brush past MAX_BRUSHES, a spawn past MAX_SPAWNS
    and a plane past MAX_BRUSH_PLANES all vanish with nothing but a debugf on a
    console nobody is watching."""
    bad = []
    nb, ns = len(state["brushes"]), len(state["spawns"])
    if nb > LIMITS["brushes"]:
        bad.append("%d brushes; kiln_map.c loads %d and DROPS the rest"
                   % (nb, LIMITS["brushes"]))
    if nb * 6 > LIMITS["faces"]:
        bad.append("%d brushes is more than %d faces, which fails the load "
                   "outright rather than dropping anything"
                   % (nb, LIMITS["faces"]))
    if ns > LIMITS["spawns"]:
        bad.append("%d spawns; kiln_map.c loads %d and DROPS the rest"
                   % (ns, LIMITS["spawns"]))
    classes = {s["classname"] for s in state["spawns"]}
    if len(classes) > LIMITS["classnames"]:
        bad.append("%d distinct classnames; the runtime table holds %d"
                   % (len(classes), LIMITS["classnames"]))

    lim = LIMITS["coord"]
    for i, b in enumerate(state["brushes"]):
        pts = b["convex"] if "convex" in b else [b["mins"], b["maxs"]]
        for p in pts:
            if any(abs(c) > lim for c in p):
                bad.append("brush %d reaches %s, past the +/-%d a face "
                           "vertex can hold (kiln_map.c packs them int16)"
                           % (i + 1, p, lim))
                break
    for s in state["spawns"]:
        if any(abs(c) > lim for c in s["origin"]):
            bad.append("%s is at %s, past +/-%d"
                       % (s["classname"], s["origin"], lim))
    return bad


def known_classnames():
    return set(level_vocab.classnames())


def check_classnames(state):
    known = known_classnames()
    return ["%s is not in the level vocabulary; the editor cannot offer it and "
            "nothing will spawn unless the game registers it"
            % cn for cn in sorted({s["classname"] for s in state["spawns"]})
            if cn not in known]


# ── state -> .tscn ──────────────────────────────────────────────────────────

TSCN_HEADER = '[gd_scene load_steps=%d format=3]\n'


def _fmt(v):
    """Enough significant digits to survive the multiply back up.

    %g is 6 SIGNIFICANT digits, not 6 decimals: it wrote 1.015625 as 1.01562,
    and at the far end of the coord range (32767/64 = 511.984375) that is the
    difference between landing on the limit and landing past it."""
    v = float(v) + 0.0                 # normalises -0.0, which prints as "-0"
    return "%.12g" % v


def state_to_scene(state, scale=64.0, name="Level"):
    """The state shape -> .tscn text, so an existing `.map` opens in the editor.

    A box brush becomes a CSGBox3D, which is the node this file reads back and
    the one that previews as solid geometry. A convex brush becomes a
    ConvexPolygonShape3D instead: it round-trips exactly, but it draws as a
    shape gizmo rather than a solid, so a level meant to be edited in Redot is
    better authored out of boxes and the convex form kept for the detail that
    does not need moving again."""
    subs, nodes = [], []
    inv = 1.0 / scale

    for i, b in enumerate(state.get("brushes", [])):
        tex = b.get("texture") or "TEX"
        if "convex" in b:
            pts = [tuple(c * inv for c in p) for p in b["convex"]]
            sid = "Convex_%d" % (i + 1)
            flat = ", ".join(_fmt(c) for p in pts for c in p)
            subs.append('[sub_resource type="ConvexPolygonShape3D" id="%s"]\n'
                        'points = PackedVector3Array(%s)\n' % (sid, flat))
            nodes.append(
                '[node name="Brush%d" type="CollisionShape3D" parent="."]\n'
                'shape = SubResource("%s")\n'
                'metadata/kiln_texture = "%s"\n' % (i + 1, sid, tex))
            continue
        mn, mx = b["mins"], b["maxs"]
        size = [(mx[k] - mn[k]) * inv for k in range(3)]
        cen = [(mx[k] + mn[k]) / 2.0 * inv for k in range(3)]
        nodes.append(
            '[node name="Brush%d" type="CSGBox3D" parent="."]\n'
            'transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, %s, %s, %s)\n'
            'size = Vector3(%s, %s, %s)\n'
            'metadata/kiln_texture = "%s"\n'
            % (i + 1, _fmt(cen[0]), _fmt(cen[1]), _fmt(cen[2]),
               _fmt(size[0]), _fmt(size[1]), _fmt(size[2]), tex))

    for i, s in enumerate(state.get("spawns", [])):
        o = [c * inv for c in s.get("origin", [0, 0, 0])]
        th = math.radians(s.get("angle", 0) or 0)
        c, sn = math.cos(th), math.sin(th)
        # The inverse of yaw_degrees: basis columns for a rotation about Y,
        # written in the column order .tscn stores.
        body = ('[node name="%s_%d" type="Marker3D" parent="."]\n'
                'transform = Transform3D(%s, 0, %s, 0, 1, 0, %s, 0, %s, '
                '%s, %s, %s)\n'
                'metadata/kiln_classname = "%s"\n'
                % (s.get("classname", "spawn"), i + 1,
                   _fmt(c), _fmt(-sn), _fmt(sn), _fmt(c),
                   _fmt(o[0]), _fmt(o[1]), _fmt(o[2]),
                   s.get("classname", "info_player_start")))
        for k, v in mapfmt._epair_items(s.get("epairs")):
            if k in ("classname", "origin", "angle"):
                continue
            body += 'metadata/kiln_epair_%s = "%s"\n' % (k, v)
        nodes.append(body)

    root = '[node name="%s" type="Node3D"]\n' % name
    for k, v in (state.get("worldspawn") or {}).items():
        root += 'metadata/kiln_worldspawn_%s = "%s"\n' % (k, v)

    out = [TSCN_HEADER % (len(subs) + 1)]
    out += subs
    out.append(root)
    out += nodes
    return "\n".join(out)


# ── CLI ─────────────────────────────────────────────────────────────────────

def to_map(tscn_text, scale):
    warn = []
    state = scene_to_state(tscn_text, scale=scale, warn=warn)
    problems = check_limits(state)
    if problems:
        die("this scene does not fit the engine:\n  - "
            + "\n  - ".join(problems))
    text = mapfmt.emit_state(state)
    # Self-validating, exactly as map-emit is: never write a .map that
    # map-validate would then reject.
    report = mapfmt.analyse(quake_map.parse_map(text))
    if report["problems"]:
        die("emitted a .map that does not validate (%s); this is a bug in "
            "tscn_map, not in the scene" % ", ".join(report["problems"]))
    return text, state, warn, report


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tscn_map.py",
                                 description=__doc__.split("\n")[0])
    ap.add_argument("--to-map", metavar="SCENE.tscn")
    ap.add_argument("--to-tscn", metavar="LEVEL.map")
    ap.add_argument("--out", required=True, help="'-' for stdout")
    ap.add_argument("--scale", type=float, default=64.0,
                    help="world units per authored metre (default 64)")
    ap.add_argument("--name", default="Level", help="root node name for --to-tscn")
    ap.add_argument("--json", action="store_true",
                    help="--to-map: write the state JSON instead of .map text")
    a = ap.parse_args(argv)

    if bool(a.to_map) == bool(a.to_tscn):
        die("give exactly one of --to-map or --to-tscn")

    if a.to_map:
        text, state, warn, report = to_map(Path(a.to_map).read_text(), a.scale)
        payload = json.dumps(state, indent=2) + "\n" if a.json else text
        if a.out == "-":
            sys.stdout.write(payload)
        else:
            Path(a.out).write_text(payload)
        for w in warn:
            print("tscn_map: warning: " + w, file=sys.stderr)
        for w in check_classnames(state):
            print("tscn_map: warning: " + w, file=sys.stderr)
        for w in report.get("aabb_only", []):
            print("tscn_map: warning: brush %d is not axis-aligned, so it "
                  "DRAWS its true shape and COLLIDES as the box %s..%s"
                  % (w["brush"], w["mins"], w["maxs"]), file=sys.stderr)
        print("tscn_map: %d brushes, %d spawns -> %s"
              % (len(state["brushes"]), len(state["spawns"]), a.out),
              file=sys.stderr)
        return 0

    text = Path(a.to_tscn).read_text()
    state = mapfmt.to_state(quake_map.parse_map(text))
    out = state_to_scene(state, scale=a.scale, name=a.name)
    if a.out == "-":
        sys.stdout.write(out)
    else:
        Path(a.out).write_text(out)
    print("tscn_map: %d brushes, %d spawns -> %s"
          % (len(state["brushes"]), len(state["spawns"]), a.out),
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
