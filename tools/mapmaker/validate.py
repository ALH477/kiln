#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""validate.py — round-trip a .map through tools/blender/quake_map.py's pure
Python core (the same CSG the Blender import path uses). Reports counts vs. the
engine's limits, flags degenerate brushes AND SAYS WHY, and verifies every spawn
has a parseable origin and an integer angle.

All of the arithmetic lives in tools/mapmaker/mapfmt.py; this file is a printer
over mapfmt.analyse(). The default stdout is held byte-identical because
nix/checks/mapmaker-roundtrip.nix greps "brushes:   1 /", "spawns:    1 /" and
"OK", and nix/checks/forge-roundtrip.nix greps "OK".

Exit codes:
    0  clean
    1  parse error, or limit/AABB/origin/angle violation
    2  bad usage

With --diff, also emit a canonical-form .map to stdout (the same corner
ordering tools/mapmaker/src/mapio.js uses) so a reviewer can diff an exported
file against the validator's view of it to spot exporter drift.

With --json, emit mapfmt.analyse()'s result as JSON on stdout and nothing else,
so the output can be piped straight into a parser.
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

# Re-exported so existing importers (and tools/blender-mcp/server.py) keep
# working against one definition rather than a second copy.
LIMITS = mapfmt.LIMITS
aabb_faces = mapfmt.aabb_faces
face_line = mapfmt.face_line
emit_canonical = mapfmt.emit_canonical

# What to tell someone whose brush did not survive the CSG. The cause is
# computed by mapfmt.diagnose_brush; this is only the remedy.
REMEDY = {
    "inside-out":
        "the planes bound a real box but are wound inward, so the half-space\n"
        "  intersection is empty. Fix losslessly with:  ./dev map-canon <file.map>",
    "not-closed":
        "the brush is not a closed convex volume, so there is no box to\n"
        "  intersect. './dev map-canon' will rebuild it from the plane points --\n"
        "  which is what kiln_map.c already uses -- but LOOK at the result.",
    "duplicate-plane":
        "two faces describe the same plane, so the brush is short a bound.\n"
        "  './dev map-canon' rebuilds it; check the source that emitted it.",
    "unknown":
        "cause not identified. Run './dev map-render <file.map>' and look.",
}


def main():
    ap = argparse.ArgumentParser(description="round-trip validate a .map")
    ap.add_argument("path", help=".map file to validate")
    ap.add_argument("--diff", action="store_true",
                    help="also print canonical-form .map to stdout")
    ap.add_argument("--json", action="store_true",
                    help="print the analysis as JSON and nothing else")
    args = ap.parse_args()

    text = Path(args.path).read_text()

    try:
        entities = quake_map.parse_map(text)
    except quake_map.MapSyntaxError as e:
        if args.json:
            json.dump({"parse_error": str(e), "exit": 1}, sys.stdout, indent=2)
            sys.stdout.write("\n")
        else:
            print(f"PARSE ERROR: {e}", file=sys.stderr)
        return 1

    r = mapfmt.analyse(entities)

    if args.json:
        json.dump(r, sys.stdout, indent=2)
        sys.stdout.write("\n")
        return r["exit"]

    print(f"entities:  {r['entities']}")
    print(f"brushes:   {r['brushes']} / {LIMITS['brushes']}")
    print(f"faces:     {r['faces']} / {LIMITS['faces']}")
    print(f"spawns:    {r['spawns']} / {LIMITS['spawns']}")
    print(f"classnames: {len(r['classnames'])} / {LIMITS['classnames']} "
          f"({r['classnames']})")

    if r["degenerate"]:
        print(f"DEGENERATE: {len(r['degenerate'])} brush(es) with "
              f"<6 surviving CSG faces:")
        for d in r["degenerate"]:
            print(f"  brush #{d['brush']}: {d['faces']} faces "
                  f"[{d['cause']}] {d['detail']}")
        # The old message stopped at the count, which told a reader that
        # something was wrong but not what or what to do -- and six of the
        # seven committed maps hit it.
        for cause in sorted({d["cause"] for d in r["degenerate"]}):
            print(f"\n{cause}: {REMEDY[cause]}")
    if r["bad_coord"]:
        print(f"OUT-OF-RANGE: {len(r['bad_coord'])} brush(es) exceed "
              f"±{LIMITS['coord']}")
    if r["bad_origin"]:
        print(f"BAD ORIGIN: {len(r['bad_origin'])} spawn(s) with "
              f"unparseable origin")
    if r["bad_angle"]:
        print(f"BAD ANGLE: {len(r['bad_angle'])} spawn(s) with "
              f"non-integer angle")

    if args.diff:
        sys.stdout.write(mapfmt.emit_canonical(entities))

    if r["problems"]:
        print(f"\nFAIL: {', '.join(r['problems'])}", file=sys.stderr)
        return 1

    if r["fps_warnings"]:
        print(f"\nFPS warnings ({len(r['fps_warnings'])}):")
        for w in r["fps_warnings"]:
            print(f"  WARN: {w}")

    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
