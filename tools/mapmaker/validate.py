#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""validate.py — round-trip a .map through tools/blender/quake_map.py's pure
Python core (the same parser the engine uses structurally, and the same one
the Blender import path uses). Reports counts vs. the engine's limits,
flags degenerate AABBs (brushes whose plane set, intersected via brush CSG,
yields fewer than 6 surviving faces), and verifies every spawn has a
parseable origin and an integer angle.

Run as a script (`python3 tools/mapmaker/validate.py <path.map>`), never as
an import — running as a script leaves no __pycache__ in the tree, which the
repo's conventions want for the mapmaker tools subdirectory.

Exit codes:
    0  clean
    1  parse error, or limit/AABB/origin/angle violation
    2  bad usage

With --diff, also emit a canonical-form .map to stdout (the same corner
ordering tools/mapmaker/src/mapio.js uses) so a reviewer can diff an
exported file against the validator's view of it to spot exporter drift.
"""

import argparse
import math
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BLENDER = HERE.parent / "blender"
sys.path.insert(0, str(BLENDER))

import quake_map  # noqa: E402

LIMITS = {
    "brushes": 256,
    "faces": 1536,
    "spawns": 64,
    "classnames": 32,
    "coord": 32767,
}

# Canonical 6-face corner ordering — MUST match tools/mapmaker/src/mapio.js
# and the winding of assets/quake_test.map:4-9 (the file mkQuakeMapModel
# round-trips through Blender, so quake_map.py's CSG accepts it). See
# vendor/README.md's VENDORED_WINDING note. oot_test.map is inside-out
# relative to this convention and would fail this validator; the engine
# accepts both because its AABB reduction is winding-independent.
def aabb_faces(mn, mx):
    xmin, ymin, zmin = mn
    xmax, ymax, zmax = mx
    return [
        ((xmin, ymin, zmin), (xmin, ymax, zmin), (xmin, ymin, zmax)),  # -X
        ((xmax, ymin, zmin), (xmax, ymin, zmax), (xmax, ymax, zmin)),  # +X
        ((xmin, ymin, zmin), (xmin, ymin, zmax), (xmax, ymin, zmin)),  # -Y
        ((xmin, ymax, zmin), (xmax, ymax, zmin), (xmin, ymax, zmax)),  # +Y
        ((xmin, ymin, zmin), (xmax, ymin, zmin), (xmin, ymax, zmin)),  # -Z
        ((xmin, ymin, zmax), (xmin, ymax, zmax), (xmax, ymin, zmax)),  # +Z
    ]


def face_line(p, tex):
    def f(v): return f"{round(v)}"
    return (f"( {f(p[0][0])} {f(p[0][1])} {f(p[0][2])} ) "
            f"( {f(p[1][0])} {f(p[1][1])} {f(p[1][2])} ) "
            f"( {f(p[2][0])} {f(p[2][1])} {f(p[2][2])} ) "
            f"{tex} 0 0 0 1 1")


def emit_canonical(entities):
    lines = ['{', '"classname" "worldspawn"']
    for ent in entities:
        for brush in ent["brushes"]:
            mn = [math.inf] * 3
            mx = [-math.inf] * 3
            tex = "TEX"
            for p in brush:
                for v in (p["p1"], p["p2"], p["p3"]):
                    for i in range(3):
                        if v[i] < mn[i]: mn[i] = v[i]
                        if v[i] > mx[i]: mx[i] = v[i]
                tex = p["texture"] or "TEX"
            lines.append("{")
            for f in aabb_faces(mn, mx):
                lines.append(face_line(f, tex))
            lines.append("}")
    lines.append("}")
    for ent in entities:
        if ent["brushes"]:
            continue
        p = ent["props"]
        lines.append('{')
        lines.append(f'"classname" "{p.get("classname", "info_player_start")}"')
        if "origin" in p:
            lines.append(f'"origin" "{p["origin"]}"')
        if "angle" in p:
            lines.append(f'"angle" "{p["angle"]}"')
        for k, v in p.items():
            if k in ("classname", "origin", "angle"):
                continue
            lines.append(f'"{k}" "{v}"')
        lines.append('}')
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description="round-trip validate a .map")
    ap.add_argument("path", help=".map file to validate")
    ap.add_argument("--diff", action="store_true",
                    help="also print canonical-form .map to stdout")
    args = ap.parse_args()

    text = Path(args.path).read_text()
    problems = []

    try:
        entities = quake_map.parse_map(text)
    except quake_map.MapSyntaxError as e:
        print(f"PARSE ERROR: {e}", file=sys.stderr)
        return 1

    n_brushes = 0
    n_faces = 0
    n_spawns = 0
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
                # AABB extents
                mn = [math.inf] * 3
                mx = [-math.inf] * 3
                for p in brush:
                    for v in (p["p1"], p["p2"], p["p3"]):
                        for i in range(3):
                            if v[i] < mn[i]: mn[i] = v[i]
                            if v[i] > mx[i]: mx[i] = v[i]
                for i in range(3):
                    if abs(mn[i]) > LIMITS["coord"] or abs(mx[i]) > LIMITS["coord"]:
                        bad_coord.append((n_brushes, mn, mx))
                # CSG — fewer than 6 surviving faces means degenerate
                faces = quake_map.brush_to_faces(brush)
                n_surv = sum(len(polys) for polys in faces.values())
                if n_surv < 6:
                    degenerate.append((n_brushes, n_surv))
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
                    bad_origin.append((n_spawns, cn, props.get("origin", "")))
            if "angle" in props:
                try:
                    int(props["angle"])
                except ValueError:
                    bad_angle.append((n_spawns, cn, props.get("angle", "")))

    print(f"entities:  {len(entities)}")
    print(f"brushes:   {n_brushes} / {LIMITS['brushes']}")
    print(f"faces:     {n_faces} / {LIMITS['faces']}")
    print(f"spawns:    {n_spawns} / {LIMITS['spawns']}")
    print(f"classnames: {len(classnames)} / {LIMITS['classnames']} ({sorted(classnames)})")
    if degenerate:
        print(f"DEGENERATE: {len(degenerate)} brush(es) with <6 surviving CSG faces:")
        for i, n in degenerate:
            print(f"  brush #{i}: {n} faces")
        problems.append("degenerate brushes")
    if bad_coord:
        print(f"OUT-OF-RANGE: {len(bad_coord)} brush(es) exceed ±{LIMITS['coord']}")
        problems.append("coord range")
    if bad_origin:
        print(f"BAD ORIGIN: {len(bad_origin)} spawn(s) with unparseable origin")
        problems.append("origin")
    if bad_angle:
        print(f"BAD ANGLE: {len(bad_angle)} spawn(s) with non-integer angle")
        problems.append("angle")
    if n_brushes > LIMITS["brushes"]: problems.append("brush limit")
    if n_faces > LIMITS["faces"]: problems.append("face limit")
    if n_spawns > LIMITS["spawns"]: problems.append("spawn limit")
    if len(classnames) > LIMITS["classnames"]: problems.append("classname limit")

    if args.diff:
        sys.stdout.write(emit_canonical(entities))

    if problems:
        print(f"\nFAIL: {', '.join(problems)}", file=sys.stderr)
        return 1

    # ── FPS entity-specific epair validation ──────────────────────────
    # Checks required epairs per classname and cross-references (switch→door,
    # key→door). Warnings, not errors — the map still parses, but gameplay
    # will be broken.
    fps_warnings = []

    # Collect spawns by classname for cross-referencing.
    spawns_by_class = {}
    for ent in entities:
        if ent["brushes"]:
            continue
        props = ent["props"]
        cn = props.get("classname", "?")
        spawns_by_class.setdefault(cn, []).append(props)

    # Required epairs per classname.
    required_epairs = {
        "info_key_door": ["key_id"],
        "info_switch": ["target_door"],
        "info_trigger": ["mins", "maxs", "event_id", "type"],
        "info_npc": ["dialogue"],
        "info_chest": ["contents"],
    }

    for cn, reqs in required_epairs.items():
        for props in spawns_by_class.get(cn, []):
            for req in reqs:
                if req not in props:
                    fps_warnings.append(
                        f"{cn} at {props.get('origin', '?')} missing '{req}' epair")

    # Cross-reference: info_switch.target_door should reference a door.
    door_ids = set()
    for props in spawns_by_class.get("info_key_door", []):
        # Doors don't have an id epair by default; use their index.
        pass
    door_count = len(spawns_by_class.get("info_key_door", []))
    for props in spawns_by_class.get("info_switch", []):
        td = props.get("target_door", "")
        if td and td != "0":
            # target_door is a 0-based index into the door list.
            try:
                idx = int(td)
                if idx < 0 or idx >= door_count:
                    fps_warnings.append(
                        f"info_switch at {props.get('origin', '?')} "
                        f"targets door index {idx} but only {door_count} door(s) exist")
            except ValueError:
                fps_warnings.append(
                    f"info_switch at {props.get('origin', '?')} "
                    f"has unparseable target_door '{td}'")

    # Cross-reference: info_key_door.key_id should match a key pickup.
    key_ids_doors = set()
    for props in spawns_by_class.get("info_key_door", []):
        kid = props.get("key_id", "")
        if kid:
            key_ids_doors.add(kid)
    key_ids_pickups = set()
    for props in spawns_by_class.get("info_key_red", []):
        key_ids_pickups.add("1")  # info_key_red always = key id 1
    for kid in key_ids_doors:
        if kid not in key_ids_pickups:
            fps_warnings.append(
                f"info_key_door requires key_id {kid} but no "
                f"info_key_red pickup exists for that key")

    # info_trigger: validate mins/maxs are parseable vec3.
    for props in spawns_by_class.get("info_trigger", []):
        for vk in ("mins", "maxs"):
            v = props.get(vk, "")
            if v:
                try:
                    parts = v.split()
                    [float(x) for x in parts]
                    if len(parts) != 3:
                        raise ValueError
                except ValueError:
                    fps_warnings.append(
                        f"info_trigger at {props.get('origin', '?')} "
                        f"has unparseable {vk} '{v}'")

    if fps_warnings:
        print(f"\nFPS warnings ({len(fps_warnings)}):")
        for w in fps_warnings:
            print(f"  WARN: {w}")

    if problems:
        return 1
    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())