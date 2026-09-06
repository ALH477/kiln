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
                if reduce == "planes":
                    mn, mx, _rule = canon_brush(brush)
                else:
                    mn, mx = box_from_points(brush)
                texs = {p.get("texture", "TEX") for p in brush}
                if len(texs) > 1:
                    print(f"mapfmt: brush #{bi} has {len(texs)} textures "
                          f"{sorted(texs)}; keeping the first face's",
                          file=sys.stderr)
                brushes.append({
                    "mins": [_num_val(v) for v in mn],
                    "maxs": [_num_val(v) for v in mx],
                    "texture": brush[0].get("texture", "TEX") if brush else "TEX",
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
                n_surv = csg_face_count(brush)
                if n_surv < 6:
                    cause, detail = diagnose_brush(brush)
                    degenerate.append({"brush": n_brushes, "faces": n_surv,
                                       "cause": cause, "detail": detail})
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
        "bad_origin": bad_origin,
        "bad_angle": bad_angle,
        "fps_warnings": _fps_warnings(entities),
        "problems": problems,
        "exit": 1 if problems else 0,
    }
