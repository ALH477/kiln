#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Did the level survive the trip out of the editor and back?

A separate file rather than a heredoc inside nix/checks/tscn-map.nix, for the
same reason level-vocab-check.py is: the nix string indents its body, and
Python is the one language where that is a syntax error rather than cosmetic.

Compares STATE, not bytes. A convex brush goes into the editor as a box plus a
rotation and comes back out as its hull, so the two files differ while the
solid does not. What has to survive is the geometry, the spawns and the
worldspawn epairs.
"""

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
for sub in ("mapmaker", "blender", "schema"):
    sys.path.insert(0, str(Path("tools") / sub))

import mapfmt       # noqa: E402
import quake_map    # noqa: E402


def state(path):
    return mapfmt.to_state(quake_map.parse_map(Path(path).read_text()))


def main(argv):
    if len(argv) != 2:
        print("usage: tscn-map-roundtrip.py BEFORE.map AFTER.map", file=sys.stderr)
        return 2
    a, b = state(argv[0]), state(argv[1])
    bad = 0

    if len(a["brushes"]) != len(b["brushes"]):
        print("FAILED: %d brushes went in, %d came back"
              % (len(a["brushes"]), len(b["brushes"])))
        bad = 1
    for i, (x, y) in enumerate(zip(a["brushes"], b["brushes"])):
        if x != y:
            print("FAILED: brush %d changed across the editor round trip" % (i + 1))
            print("  out:", x)
            print("  in :", y)
            bad = 1

    ka = [(s["classname"], s["origin"], s["angle"]) for s in a["spawns"]]
    kb = [(s["classname"], s["origin"], s["angle"]) for s in b["spawns"]]
    if ka != kb:
        print("FAILED: spawns changed")
        print("  out:", ka)
        print("  in :", kb)
        bad = 1

    if a.get("worldspawn") != b.get("worldspawn"):
        print("FAILED: worldspawn epairs changed: %s -> %s"
              % (a.get("worldspawn"), b.get("worldspawn")))
        bad = 1
    return bad


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
