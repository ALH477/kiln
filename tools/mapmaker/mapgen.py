#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""mapgen.py — author, read and canonicalise a Quake .map from the shell.

The browser editor (tools/mapmaker/) can only hand a file to a human through a
download. This is the same dialect, for anything that has no browser: a build
script, a test, or an agent. Both speak tools/mapmaker/mapfmt.py, so there is
no second statement of the winding table, the limits, or the entity rules.

    mapgen emit    <spec.json|-> [out.map|-]   JSON -> .map, self-validating
    mapgen dump    <in.map>      [out.json|-]  .map -> JSON
    mapgen canon   <in.map>      [out.map]     re-emit in canonical winding
    mapgen example [--fps]                     a worked spec on stdout
    mapgen classes                             the entity/epair table

The spec is the browser editor's own state shape, so a file dumped here can be
pasted into the editor and back:

    {"brushes": [{"mins": [-64,0,-64], "maxs": [64,32,64], "texture": "TEX"}],
     "spawns":  [{"classname": "info_player_start", "origin": [0,8,0],
                  "angle": 0, "epairs": {}}]}

Exit codes: 0 clean, 1 bad content, 2 bad usage.
"""

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "blender"))

import mapfmt      # noqa: E402
import quake_map   # noqa: E402


def _read(path):
    return sys.stdin.read() if path == "-" else Path(path).read_text()


def _write(path, text):
    if path is None or path == "-":
        sys.stdout.write(text)
    else:
        Path(path).write_text(text)
        print(f"wrote {path}", file=sys.stderr)


def _report(r, where):
    """Print analyse()'s problems the way validate.py would, to stderr."""
    print(f"{where}: {r['brushes']} brushes, {r['spawns']} spawns", file=sys.stderr)
    for d in r["degenerate"]:
        print(f"  brush #{d['brush']}: {d['faces']} faces "
              f"[{d['cause']}] {d['detail']}", file=sys.stderr)
    for w in r["fps_warnings"]:
        print(f"  WARN: {w}", file=sys.stderr)
    if r["problems"]:
        print(f"FAIL: {', '.join(r['problems'])}", file=sys.stderr)


def cmd_emit(args):
    try:
        spec = json.loads(_read(args.spec))
    except json.JSONDecodeError as e:
        print(f"mapgen: {args.spec} is not valid JSON: {e}", file=sys.stderr)
        return 1
    if not isinstance(spec, dict) or "brushes" not in spec:
        print("mapgen: spec needs a top-level {\"brushes\": [...]} — "
              "run 'mapgen example' for one", file=sys.stderr)
        return 1

    # Reject a degenerate box up front: naming the axis beats reporting it
    # later as a CSG failure the author has to work backwards from.
    for i, b in enumerate(spec["brushes"]):
        mn, mx = b.get("mins"), b.get("maxs")
        if not (isinstance(mn, list) and isinstance(mx, list)
                and len(mn) == 3 and len(mx) == 3):
            print(f"mapgen: brush #{i} needs mins and maxs as 3-element lists",
                  file=sys.stderr)
            return 1
        for a in range(3):
            if mn[a] >= mx[a]:
                print(f"mapgen: brush #{i} is degenerate on {'xyz'[a]}: "
                      f"mins[{a}]={mn[a]} >= maxs[{a}]={mx[a]}", file=sys.stderr)
                return 1

    text = mapfmt.emit_state(spec)

    # Validate our OWN output before writing it. An author should not be able
    # to produce a broken .map and find out two commands later.
    r = mapfmt.analyse(quake_map.parse_map(text))
    if r["problems"]:
        _report(r, "mapgen: emitted map does not validate")
        return 1
    _write(args.out, text)
    if r["fps_warnings"]:
        _report(r, "note")
    return 0


def cmd_dump(args):
    text = _read(args.map)
    try:
        entities = quake_map.parse_map(text)
    except quake_map.MapSyntaxError as e:
        print(f"mapgen: {args.map}: {e}", file=sys.stderr)
        return 1
    state = mapfmt.to_state(entities)

    # Warn when a re-emit would not reproduce the input's own coordinates.
    # assets/quake_test.map uses the one-unit-apart plane-point convention, so
    # its AABB reads back one unit larger than authored through the points; an
    # author who dumps, moves one wall and re-emits would silently move the
    # others. Not hypothetical -- it is the off-by-one kiln-map.nix pins.
    if mapfmt.emit_state(state) != text:
        print(f"note: re-emitting {args.map} will not reproduce it byte for "
              f"byte (comments, spacing or the plane-point convention differ)",
              file=sys.stderr)
    _write(args.out, json.dumps(state, indent=2) + "\n")
    return 0


def cmd_canon(args):
    text = _read(args.map)
    entities = quake_map.parse_map(text)

    # Report which reduction each brush needed. "planes" recovers the authored
    # box exactly; "points" means the brush was not a closed volume and we fell
    # back to what kiln_map.c already computes for it -- which is safe for every
    # runtime consumer, and worth reading, because it says the source was wrong.
    rules = {}
    for ent in entities:
        for brush in ent["brushes"]:
            _mn, _mx, rule = mapfmt.canon_brush(brush)
            rules[rule] = rules.get(rule, 0) + 1
    for rule, n in sorted(rules.items()):
        note = ("recovered from the plane distances" if rule == "planes"
                else "NOT a closed volume — rebuilt from the plane points")
        print(f"  {n} brush(es): {note}", file=sys.stderr)

    out = mapfmt.emit_canonical(entities, reduce="planes")
    r = mapfmt.analyse(quake_map.parse_map(out))
    if r["problems"]:
        _report(r, "mapgen: canonical form still does not validate")
        return 1
    _write(args.out or args.map, out)
    return 0


EXAMPLE = {
    "brushes": [
        {"mins": [-128, -8, -128], "maxs": [128, 0, 128], "texture": "FLOOR"},
        {"mins": [-128, 0, -128], "maxs": [128, 64, -120], "texture": "WALL"},
        {"mins": [-128, 0, 120], "maxs": [128, 64, 128], "texture": "WALL"},
    ],
    "spawns": [
        {"classname": "info_player_start", "origin": [0, 16, 0], "angle": 0,
         "epairs": {}},
    ],
}

EXAMPLE_FPS = {
    "brushes": [
        {"mins": [-128, -8, -128], "maxs": [128, 0, 128], "texture": "FLOOR"},
    ],
    "spawns": [
        {"classname": "info_player_start", "origin": [0, 16, 0], "angle": 0,
         "epairs": {}},
        {"classname": "info_key_red", "origin": [64, 8, 0], "angle": 0,
         "epairs": {}},
        {"classname": "info_key_door", "origin": [-64, 8, 0], "angle": 90,
         "epairs": {"key_id": "1"}},
    ],
}


def cmd_example(args):
    spec = EXAMPLE_FPS if args.fps else EXAMPLE
    sys.stdout.write(json.dumps(spec, indent=2) + "\n")
    return 0


def cmd_classes(args):
    print("classname            required epairs")
    print("-------------------- ------------------------------------")
    for cn in sorted(mapfmt.REQUIRED_EPAIRS):
        print(f"{cn:20} {', '.join(mapfmt.REQUIRED_EPAIRS[cn])}")
    print("\nAny other classname is accepted by the parser; the engine skips")
    print("one no game registered via kiln_map_register_classname.")
    return 0


def main():
    ap = argparse.ArgumentParser(
        prog="mapgen", description=__doc__.split("\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("emit", help="JSON spec -> .map")
    p.add_argument("spec"); p.add_argument("out", nargs="?")
    p.set_defaults(fn=cmd_emit)

    p = sub.add_parser("dump", help=".map -> JSON spec")
    p.add_argument("map"); p.add_argument("out", nargs="?")
    p.set_defaults(fn=cmd_dump)

    p = sub.add_parser("canon", help="re-emit a .map in canonical winding")
    p.add_argument("map"); p.add_argument("out", nargs="?")
    p.set_defaults(fn=cmd_canon)

    p = sub.add_parser("example", help="print a worked spec")
    p.add_argument("--fps", action="store_true", help="an FPS entity example")
    p.set_defaults(fn=cmd_example)

    p = sub.add_parser("classes", help="the entity/epair table")
    p.set_defaults(fn=cmd_classes)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
