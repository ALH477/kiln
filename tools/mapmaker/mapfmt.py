#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""mapfmt.py — the one Python statement of Kiln's `.map` dialect.

Everything here was MOVED out of tools/mapmaker/validate.py rather than copied,
so there is exactly one Python copy of the canonical face table, the engine's
limits, and the FPS entity rules. `validate.py` is now a printer over
`analyse()`; `mapgen.py` is an author/reader over `to_state()`/`emit_state()`.

The JS twin is tools/mapmaker/src/mapio.js. Where the two used to disagree,
this file adopts the JS behaviour, because mapio.js is the older emitter and
the one the browser editor actually ships:
  * a spawn's "origin" and "angle" are emitted unconditionally, defaulting to
    "0 0 0" and 0 — validate.py used to emit them only when present.
  * a brush's texture is taken from its FIRST face — validate.py used to take
    the last, because its loop overwrote.
Neither difference is observable in a file the editor wrote; both are
observable in a hand-authored one, which is exactly the case an agent hits.

── Why the winding matters ─────────────────────────────────────────────
tools/blender/quake_map.py derives a face's outward normal as
cross(p3-p1, p2-p1). Get it backwards and the half-space intersection is
empty: the brush yields ZERO polygons through the CSG while loading perfectly
on console, because kiln_map.c only ever takes the componentwise min/max of
the plane points and is indifferent to both winding and closure. That failure
is silent in the only place anyone looks, which is why six of the seven
committed .map files were inside-out before anything reported it.
"""

import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "blender"))
sys.path.insert(0, str(HERE.parent / "schema"))

import quake_map     # noqa: E402
import level_vocab   # noqa: E402

EPS = 1e-6

# Everything below comes from tools/schema/level_vocab.json, which is the one
# place any of it is written. See tools/schema/level_vocab.py for what used to
# be where.
LIMITS = level_vocab.limits()
REQUIRED_EPAIRS = level_vocab.required_epairs()


# ── the canonical face table ────────────────────────────────────────────────
# Corner ordering for the 6 AABB faces. MUST match tools/mapmaker/src/mapio.js's
# aabbFaces and the winding of assets/quake_test.map. See the module docstring.


def aabb_faces(mn, mx):
    """Built from the schema's face table, not transcribed. This was one of
    four hand-kept copies; the others were mapio.js, frg.py and forge_io.c."""
    return level_vocab.aabb_faces(mn, mx)


def _num_val(v):
    """The numeric value, int where integral. to_state's twin of _num."""
    r = round(v)
    return r if abs(v - r) < 1e-9 else v


def _num(v):
    """Integers stay integers; a fractional coordinate stays fractional.

    mapio.js rounds unconditionally (its fmt() is Math.round), which is right
    for the browser editor because it snaps every brush to a grid -- so for any
    map the editor wrote, this produces byte-identical output. It is wrong for
    a hand-authored file: assets/hangar.map has a 0.4-thick floor slab, and
    rounding collapses it to zero thickness, which then fails the CSG as a
    degenerate plane. Diverging from the JS here loses nothing the JS could
    have expressed and preserves geometry the JS would silently destroy."""
    r = round(v)
    if abs(v - r) < 1e-9:
        return f"{r}"
    return f"{v:g}"


def face_line(p, tex):
    f = _num
    return (f"( {f(p[0][0])} {f(p[0][1])} {f(p[0][2])} ) "
            f"( {f(p[1][0])} {f(p[1][1])} {f(p[1][2])} ) "
            f"( {f(p[2][0])} {f(p[2][1])} {f(p[2][2])} ) "
            f"{tex} 0 0 0 1 1")


def _synth_planes(mn, mx, tex):
    """The 6 canonical faces as quake_map plane dicts, so they can be fed
    straight back through brush_to_faces without a round-trip through text."""
    return [{"p1": t[0], "p2": t[1], "p3": t[2], "texture": tex,
             "xoff": 0.0, "yoff": 0.0, "rot": 0.0, "xscale": 1.0, "yscale": 1.0}
            for t in aabb_faces(mn, mx)]


def csg_face_count(brush):
    """Surviving polygons through quake_map's CSG. brush_to_faces returns a
    dict KEYED BY TEXTURE, so len() on it counts textures, not faces."""
    return sum(len(polys) for polys in quake_map.brush_to_faces(brush).values())


# ── convex brushes ──────────────────────────────────────────────────────────
# The box form is the whole vocabulary mapio.js shipped with, and it is still
# the right default. kiln_clip collides EVERY brush as its AABB (kiln_clip.h's
# header: "non-cube brushes lose internal corners"), so a brush that is not
# axis-aligned renders its true shape and blocks a bounding box. The convex
# form is for what that trade suits -- angled walls, pillars, buttresses,
# ceiling forms -- and analyse() warns on every one, so the author is told once
# per brush rather than discovering it by walking into thin air.
#
# The authored form is a POINT SET, never planes. That is the same reasoning
# tools/mapmaker/README.md gives for map-emit existing at all: an inside-out
# brush loads on console, collides correctly and draws NOTHING, and six of this
# repo's seven committed .map files were wound inward before anything noticed.
# An author who never states a winding cannot get one wrong -- so the outward
# normal is derived here, away from the hull's own centroid, which needs no
# convention to agree on.

# Looser than EPS (a hull's corners arrive through a Godot Transform3D, so they
# carry float noise) and far tighter than kiln_map.c's own CSG_EPS_ONPLANE of
# 1/32, so a hull this accepts is one the console also resolves.
HULL_EPS = 1e-3


def _pt_key(p):
    return tuple(round(c, 4) for c in p)


def _dedupe_points(points):
    pts = []
    for p in points:
        p = tuple(float(c) for c in p)
        if len(p) != 3:
            raise SystemExit("mapfmt: a convex brush point is not 3 numbers")
        if not any(_pt_key(p) == _pt_key(q) for q in pts):
            pts.append(p)
    return pts


def _spanning_triple(on_plane, nrm):
    """Three non-collinear points of a face, wound CCW as seen from outside.

    The WIDEST such triple, not the first one found. These three points are
    rounded to integers on the way out and are all the file keeps of the
    plane, so the plane is re-derived from them on the way back in: a triple
    spanning two adjacent corners of a long face tilts far more under a
    half-unit rounding nudge than one spanning the face's full diagonal.
    Maximising the cross product is maximising that lever arm."""
    if len(on_plane) < 3:
        return None
    best = None
    best_area = 0.0
    for ai in range(len(on_plane)):
        for bi in range(ai + 1, len(on_plane)):
            for ci in range(bi + 1, len(on_plane)):
                a, b, c = on_plane[ai], on_plane[bi], on_plane[ci]
                m = quake_map._cross(quake_map._sub(b, a), quake_map._sub(c, a))
                area = quake_map._len(m)
                if area <= best_area:
                    continue                  # collinear, or no wider
                if quake_map._dot(m, nrm) < 0:
                    b, c = c, b
                best, best_area = (a, b, c), area
    return best


def _hull_once(points, tex):
    """One pass of the hull: a convex point set -> quake_map plane dicts.

    Every triple of points spans a candidate plane, and that plane bounds the
    hull when every other point lies on one side of it. This is the same
    candidate-then-test shape quake_map.brush_to_faces and kiln_map.c's CSG
    already run in the other direction (planes -> vertices), run backwards --
    so there is one algorithm style in this tree rather than a hull library's
    second one. O(n^3) is free at a brush's handful of corners.
    """
    pts = _dedupe_points(points)
    if len(pts) < 4:
        raise SystemExit("mapfmt: a convex brush needs 4+ distinct points, "
                         "got %d" % len(pts))

    cen = tuple(sum(c) / len(pts) for c in zip(*pts))
    found = {}
    n = len(pts)
    for i in range(n):
        for j in range(i + 1, n):
            for k in range(j + 1, n):
                nrm = quake_map._norm(quake_map._cross(
                    quake_map._sub(pts[j], pts[i]),
                    quake_map._sub(pts[k], pts[i])))
                if nrm is None:
                    continue                  # collinear triple
                d = quake_map._dot(nrm, pts[i])
                # A hull face never contains the centroid, so "away from the
                # centroid" fixes the sign with no winding convention.
                if quake_map._dot(nrm, cen) > d:
                    nrm = tuple(-c for c in nrm)
                    d = -d
                if any(quake_map._dot(nrm, p) > d + HULL_EPS for p in pts):
                    continue                  # cuts the solid: not a face
                key = tuple(round(c, 3) for c in nrm) + (round(d, 3),)
                found.setdefault(key, (nrm, d))

    if len(found) < 4:
        raise SystemExit(
            "mapfmt: a convex brush's %d points bound %d planes; 4+ are needed "
            "for a closed volume (coplanar points enclose nothing)"
            % (len(pts), len(found)))
    if len(found) > LIMITS["brush_planes"]:
        raise SystemExit(
            "mapfmt: a convex brush needs %d planes; kiln_map.c holds %d and "
            "DROPS the rest, which leaves the solid open to walk out of"
            % (len(found), LIMITS["brush_planes"]))

    # Sorted, so the emitted plane ORDER is a pure function of the geometry.
    # Without this a dict's insertion order (which triple of points happened to
    # span each face first) reached the file, and emit|parse|emit was not
    # byte-stable -- the one property mapmaker-roundtrip.nix exists to hold.
    planes = []
    for nrm, d in sorted(found.values(), key=lambda nd: (nd[0], nd[1])):
        on = [p for p in pts if abs(quake_map._dot(nrm, p) - d) < HULL_EPS]
        tri = _spanning_triple(on, nrm)
        if tri is None:
            continue
        a, b, c = (tuple(round(v) for v in q) for q in tri)
        # plane_normal_dist reads cross(p3-p1, p2-p1), so a triple wound CCW
        # from outside is emitted with p2 and p3 swapped.
        planes.append({"p1": a, "p2": c, "p3": b, "texture": tex,
                       "xoff": 0.0, "yoff": 0.0, "rot": 0.0,
                       "xscale": 1.0, "yscale": 1.0})

    # map-emit's contract is that it cannot write a .map map-validate would
    # reject. A hull is exactly where that could stop being true, so run the
    # CSG the console and Blender both run and insist it comes back whole.
    polys = [p for ps in quake_map.brush_to_faces(planes).values() for p in ps]
    if len(polys) != len(planes):
        raise SystemExit(
            "mapfmt: a convex brush's %d planes yielded %d CSG faces; the "
            "point set is not convex" % (len(planes), len(polys)))
    worst = max((len(p) for p in polys), default=0)
    if worst > LIMITS["face_verts"]:
        raise SystemExit(
            "mapfmt: a convex brush has a %d-vertex face; kiln_map.c holds %d "
            "and drops the corners past it" % (worst, LIMITS["face_verts"]))
    return planes


def hull_planes(points, tex):
    """A convex point set -> the canonical, outward-wound, integer-coordinate
    planes for it.

    Why this iterates. A plane reaches the file as three integer points and is
    re-derived from them on the way back in, so the authored hull and the hull
    the file describes are not quite the same solid: a Redot Transform3D hands
    us corners like 55.4256, and rounding those moves every plane a little.
    One pass is therefore not a fixed point -- emit, re-read and re-emit used to
    produce a second, different file, and only the third was stable. That is
    exactly the byte-stability mapmaker-roundtrip.nix exists to hold, and it
    would have shown up as git churn on every map-dump | map-emit.

    So the canonical form of a convex brush is DEFINED as the hull of the
    integer vertices it resolves to, and that is a fixed point reached by
    applying the pass until it repeats. Two passes settle every shape tested;
    the cap is here because an unbounded loop on a pathological hull is worse
    than a clear refusal."""
    planes = _hull_once(points, tex)
    for _ in range(4):
        pts = sorted(_dedupe_points(
            v for polys in quake_map.brush_to_faces(planes).values()
            for poly in polys for v in poly))
        nxt = _hull_once(pts, tex)
        if [(q["p1"], q["p2"], q["p3"]) for q in nxt] == \
           [(q["p1"], q["p2"], q["p3"]) for q in planes]:
            return planes
        planes = nxt
    raise SystemExit(
        "mapfmt: a convex brush's hull did not settle to integer coordinates "
        "in 4 passes; its corners are probably closer together than the one "
        "unit the file can express")


def brush_points(brush):
    """The true CSG vertices of a brush, deduped -- the convex form's twin of
    box_from_points. Empty when the brush yields no solid (inside-out, or not
    closed), which is the caller's cue to fall back to a box."""
    verts = [v for polys in quake_map.brush_to_faces(brush).values()
             for poly in polys for v in poly]
    # Sorted for the same reason hull_planes sorts: the hull is a SET, and the
    # order the CSG happened to walk it must not reach the file.
    return sorted(_dedupe_points(verts))

# ── brush reduction ─────────────────────────────────────────────────────────

def box_from_points(brush):
    """Componentwise min/max of every plane point — exactly what kiln_map.c
    computes, and therefore what every runtime consumer already sees. Works on
    any brush, well-formed or not, because it never looks at a normal."""
    mn = [math.inf] * 3
    mx = [-math.inf] * 3
    for p in brush:
        for v in (p["p1"], p["p2"], p["p3"]):
            for i in range(3):
                if v[i] < mn[i]:
                    mn[i] = v[i]
                if v[i] > mx[i]:
                    mx[i] = v[i]
    return mn, mx


def box_from_planes(brush):
    """The box the six planes actually bound, ignoring their winding. Returns
    None unless every axis carries exactly two axis-aligned planes.

    This is the better reduction where it applies: it recovers the authored box
    with no off-by-one, whereas box_from_points inherits the one-unit
    plane-point convention (assets/quake_test.map's -64..64 brush reads as
    -64..65 through the points, which is the discrepancy kiln-map.nix pins)."""
    lo = [None] * 3
    hi = [None] * 3
    for p in brush:
        n, d = quake_map.plane_normal_dist(p)
        ax = max(range(3), key=lambda i: abs(n[i]))
        if abs(n[ax]) < EPS:
            return None
        # Non-axis-aligned planes cannot be reduced this way.
        if sum(1 for c in n if abs(c) < EPS) != 2:
            return None
        val = d / n[ax]
        if lo[ax] is None:
            lo[ax] = val
        elif hi[ax] is None:
            hi[ax] = val
        else:
            return None  # three planes on one axis
    if any(v is None for v in lo + hi):
        return None
    mn = [min(lo[i], hi[i]) for i in range(3)]
    mx = [max(lo[i], hi[i]) for i in range(3)]
    if any(mx[i] - mn[i] < EPS for i in range(3)):
        return None
    return mn, mx


def canon_brush(brush):
    """(mins, maxs, rule) for a brush, canonicalised. `rule` is "planes" when
    the six planes bound a real box (the authored intent, off-by-one dropped)
    and "points" when they do not and we fall back to what kiln_map.c already
    computes. The rule is reported rather than hidden because "this brush was
    not a closed volume" is the interesting half of the answer."""
    box = box_from_planes(brush)
    if box is not None:
        return box[0], box[1], "planes"
    mn, mx = box_from_points(brush)
    return mn, mx, "points"


def _same_dir(a, b):
    return all(abs(a[i] - b[i]) < 1e-4 for i in range(3))


def diagnose_brush(brush):
    """Why did this brush yield fewer than 6 CSG faces? (cause, detail).

    The three causes want three different fixes, which is why the old bare
    "DEGENERATE" message was not actionable."""
    planes = [quake_map.plane_normal_dist(p) for p in brush]
    for i in range(len(planes)):
        for j in range(i + 1, len(planes)):
            ni, di = planes[i]
            nj, dj = planes[j]
            if _same_dir(ni, nj) and abs(di - dj) < 1e-4:
                return ("duplicate-plane",
                        f"faces {i} and {j} describe the same plane")
    box = box_from_planes(brush)
    if box is None:
        # Only reason in terms of opposing AXIAL pairs when the brush was
        # trying to be axis-aligned. A wedge or a rotated wall has no such
        # pairs by design, and telling its author that "axis x lacks an
        # opposing pair" sends them looking for a wall that was never missing.
        axial = sum(1 for nrm, _ in planes
                    if sum(1 for c in nrm if abs(c) < EPS) == 2)
        if axial < len(planes):
            return ("not-closed",
                    f"not a closed convex volume; {len(planes) - axial} of "
                    f"{len(planes)} planes are not axis-aligned, so these "
                    f"half-spaces enclose nothing")
        axes = {}
        for n, d in planes:
            ax = max(range(3), key=lambda i: abs(n[i]))
            axes.setdefault("xyz"[ax], 0)
            axes["xyz"[ax]] += 1
        missing = [a for a in "xyz" if axes.get(a, 0) != 2]
        return ("not-closed",
                "not a closed convex volume; "
                f"axis {'/'.join(missing)} lacks an opposing pair of planes")
    tex = brush[0].get("texture", "TEX")
    if csg_face_count(_synth_planes(box[0], box[1], tex)) >= 6:
        return ("inside-out",
                "the planes bound a real box but are wound inward")
    return ("unknown", "planes bound a box the CSG still rejects")


# ── editor state <-> entities ───────────────────────────────────────────────

def to_state(entities, reduce="points"):
    """Flatten parsed entities into the editor's state shape.

    reduce="points" (the default) is the Python twin of mapio.js's
    toEditorState: componentwise min/max of the plane points, which is also
    exactly what kiln_map.c computes. Use it for anything that must agree with
    the JS emitter or with the engine.

    reduce="planes" is the REPAIR reduction, used by `mapgen canon`: it takes
    the box the six planes actually bound, which recovers the authored geometry
    and drops the one-unit plane-point convention along with it. That is a
    deliberate change of coordinates, so it never happens by default -- a round
    trip must not quietly move a wall."""
    brushes = []
    spawns = []
    worldspawn = {}
    for ent in entities:
        if ent["brushes"]:
            # Worldspawn's own epairs (sky, ambient light level, a game's
            # per-level settings) used to be dropped on the floor here, because
            # this branch only ever looked at the brushes. No map in this repo
            # carries any today, which is exactly why it went unnoticed.
            for k, v in ent["props"].items():
                if k != "classname":
                    worldspawn.setdefault(k, v)
            for bi, brush in enumerate(ent["brushes"]):
                texs = {p.get("texture", "TEX") for p in brush}
                if len(texs) > 1:
                    print(f"mapfmt: brush #{bi} has {len(texs)} textures "
                          f"{sorted(texs)}; keeping the first face's",
                          file=sys.stderr)
                tex = brush[0].get("texture", "TEX") if brush else "TEX"

                # A brush the six-axial-planes reduction cannot describe is not
                # a box, and flattening it to its AABB here MOVED GEOMETRY: a
                # dump|emit round trip of a 45-degree wall used to come back a
                # square block, silently. Carry the real CSG hull instead, and
                # only fall back to the box when the brush yields no solid at
                # all (inside-out, or not closed) -- which is the case
                # `mapgen canon` exists to repair.
                if box_from_planes(brush) is None:
                    pts = brush_points(brush)
                    if len(pts) >= 4:
                        brushes.append({
                            "convex": [[_num_val(c) for c in v] for v in pts],
                            "texture": tex,
                        })
                        continue

                if reduce == "planes":
                    mn, mx, _rule = canon_brush(brush)
                else:
                    mn, mx = box_from_points(brush)
                brushes.append({
                    "mins": [_num_val(v) for v in mn],
                    "maxs": [_num_val(v) for v in mx],
                    "texture": tex,
                })
        else:
            props = dict(ent["props"])
            cn = props.pop("classname", "info_player_start")
            origin = [float(x) for x in props.pop("origin", "0 0 0").split()[:3]]
            while len(origin) < 3:
                origin.append(0.0)
            try:
                angle = int(float(props.pop("angle", 0)))
            except ValueError:
                angle = 0
            spawns.append({"classname": cn, "origin": origin,
                           "angle": angle, "epairs": props})
    out = {"brushes": brushes, "spawns": spawns}
    if worldspawn:
        out["worldspawn"] = worldspawn
    return out


def _epair_items(epairs):
    """Accept both shapes: a dict (what an LLM writes) and mapio.js's
    [{k,v}] list."""
    if epairs is None:
        return []
    if isinstance(epairs, dict):
        return list(epairs.items())
    return [(e["k"], e["v"]) for e in epairs]


def emit_state(state):
    """The editor state -> .map text. The Python twin of mapio.js's emitMap."""
    lines = ["{", '"classname" "worldspawn"']
    for k, v in _epair_items(state.get("worldspawn")):
        if k != "classname":
            lines.append(f'"{k}" "{v}"')
    for b in state.get("brushes", []):
        tex = b.get("texture") or "TEX"
        lines.append("{")
        if "convex" in b:
            # The hull derives its own outward normals, so this branch cannot
            # emit an inward-wound brush any more than the box branch can.
            for pl in hull_planes(b["convex"], tex):
                lines.append(face_line((pl["p1"], pl["p2"], pl["p3"]), tex))
        else:
            for f in aabb_faces(b["mins"], b["maxs"]):
                lines.append(face_line(f, tex))
        lines.append("}")
    lines.append("}")
    for s in state.get("spawns", []):
        lines.append("{")
        lines.append(f'"classname" "{s.get("classname", "info_player_start")}"')
        o = s.get("origin", [0, 0, 0])
        lines.append(f'"origin" "{_num(o[0])} {_num(o[1])} {_num(o[2])}"')
        lines.append(f'"angle" "{round(s.get("angle", 0))}"')
        for k, v in _epair_items(s.get("epairs")):
            if k in ("classname", "origin", "angle"):
                continue
            lines.append(f'"{k}" "{v}"')
        lines.append("}")
    return "\n".join(lines) + "\n"


def emit_canonical(entities, reduce="points"):
    return emit_state(to_state(entities, reduce))


# ── analysis ────────────────────────────────────────────────────────────────

def _fps_warnings(entities):
    """Required epairs per classname plus the three cross-reference rules.
    Warnings, not errors: the map still parses, but gameplay will be broken."""
    warnings = []
    by_class = {}
    for ent in entities:
        if ent["brushes"]:
            continue
        props = ent["props"]
        by_class.setdefault(props.get("classname", "?"), []).append(props)

    for cn, reqs in REQUIRED_EPAIRS.items():
        for props in by_class.get(cn, []):
            for req in reqs:
                if req not in props:
                    warnings.append(
                        f"{cn} at {props.get('origin', '?')} missing '{req}' epair")

    # info_switch.target_door is a 0-based index into the door list.
    door_count = len(by_class.get("info_key_door", []))
    for props in by_class.get("info_switch", []):
        td = props.get("target_door", "")
        if td and td != "0":
            try:
                idx = int(td)
                if idx < 0 or idx >= door_count:
                    warnings.append(
                        f"info_switch at {props.get('origin', '?')} targets door "
                        f"index {idx} but only {door_count} door(s) exist")
            except ValueError:
                warnings.append(
                    f"info_switch at {props.get('origin', '?')} has unparseable "
                    f"target_door '{td}'")

    # info_key_door.key_id must match a pickup. info_key_red is always key 1.
    pickups = {"1"} if by_class.get("info_key_red") else set()
    for props in by_class.get("info_key_door", []):
        kid = props.get("key_id", "")
        if kid and kid not in pickups:
            warnings.append(
                f"info_key_door requires key_id {kid} but no info_key_red "
                f"pickup exists for that key")

    for props in by_class.get("info_trigger", []):
        for vk in ("mins", "maxs"):
            v = props.get(vk, "")
            if not v:
                continue
            parts = v.split()
            try:
                [float(x) for x in parts]
                if len(parts) != 3:
                    raise ValueError
            except ValueError:
                warnings.append(
                    f"info_trigger at {props.get('origin', '?')} has "
                    f"unparseable {vk} '{v}'")
    return warnings


def analyse(entities):
    """Everything validate.py used to compute inline, as data. Callers print
    it (validate.py), serialise it (--json), or act on it (mapgen emit)."""
    n_brushes = n_faces = n_spawns = 0
    classnames = set()
    degenerate = []
    bad_origin = []
    bad_angle = []
    bad_coord = []
    over_limit = []
    aabb_only = []

    for ent in entities:
        if ent["brushes"]:
            for brush in ent["brushes"]:
                n_brushes += 1
                n_faces += len(brush)
                mn, mx = box_from_points(brush)
                for i in range(3):
                    if abs(mn[i]) > LIMITS["coord"] or abs(mx[i]) > LIMITS["coord"]:
                        bad_coord.append({"brush": n_brushes, "mins": mn, "maxs": mx})
                        break
                # ONE CSG pass, for both the face count and the per-face vertex
                # count. kiln_map.c holds at most `brush_planes` planes per brush
                # and `face_verts` vertices per face, and drops the excess: past
                # the first the solid is OPEN (walk out of it), past the second a
                # face loses a corner. Both only debugf on the console -- so
                # refuse such a brush here, where the author can still see why.
                polys = [p for ps in quake_map.brush_to_faces(brush).values()
                         for p in ps]
                n_surv = len(polys)
                most_verts = max((len(p) for p in polys), default=0)
                if len(brush) > LIMITS["brush_planes"] or most_verts > LIMITS["face_verts"]:
                    over_limit.append({"brush": n_brushes, "planes": len(brush),
                                       "max_face_verts": most_verts})
                # Against the brush's OWN plane count, not against 6. A
                # convex brush is not a box: a wedge has 5 planes and 5 faces
                # and is perfectly healthy, and `< 6` called every ramp in the
                # tree degenerate.
                if n_surv < len(brush):
                    cause, detail = diagnose_brush(brush)
                    degenerate.append({"brush": n_brushes, "faces": n_surv,
                                       "cause": cause, "detail": detail})
                # Healthy but worth saying once: this brush draws its true
                # shape and COLLIDES AS ITS BOUNDING BOX, because FigBrush is
                # mins/maxs only (kiln_clip.h). Not a problem -- a warning, so
                # nothing walkable gets authored on a slope by accident.
                elif box_from_planes(brush) is None:
                    mn, mx = box_from_points(brush)
                    aabb_only.append({"brush": n_brushes, "planes": len(brush),
                                      "mins": [_num_val(v) for v in mn],
                                      "maxs": [_num_val(v) for v in mx]})
        else:
            n_spawns += 1
            props = ent["props"]
            cn = props.get("classname", "?")
            classnames.add(cn)
            if "origin" in props:
                try:
                    parts = props["origin"].split()
                    if len(parts) != 3:
                        raise ValueError
                    [float(x) for x in parts]
                except ValueError:
                    bad_origin.append({"spawn": n_spawns, "classname": cn,
                                       "origin": props.get("origin", "")})
            if "angle" in props:
                try:
                    int(props["angle"])
                except ValueError:
                    bad_angle.append({"spawn": n_spawns, "classname": cn,
                                      "angle": props.get("angle", "")})

    problems = []
    if degenerate:
        problems.append("degenerate brushes")
    if bad_coord:
        problems.append("coord range")
    if bad_origin:
        problems.append("origin")
    if bad_angle:
        problems.append("angle")
    if over_limit:
        problems.append("brush plane/vertex limit")
    if n_brushes > LIMITS["brushes"]:
        problems.append("brush limit")
    if n_faces > LIMITS["faces"]:
        problems.append("face limit")
    if n_spawns > LIMITS["spawns"]:
        problems.append("spawn limit")
    if len(classnames) > LIMITS["classnames"]:
        problems.append("classname limit")

    return {
        "entities": len(entities),
        "brushes": n_brushes,
        "faces": n_faces,
        "spawns": n_spawns,
        "classnames": sorted(classnames),
        "limits": LIMITS,
        "degenerate": degenerate,
        "bad_coord": bad_coord,
        "over_limit": over_limit,
        "aabb_only": aabb_only,
        "bad_origin": bad_origin,
        "bad_angle": bad_angle,
        "fps_warnings": _fps_warnings(entities),
        "problems": problems,
        "exit": 1 if problems else 0,
    }
