#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""manifest_check.py — hold Kiln Studio's project manifest to the repository.

    manifest_check.py <manifest.json> <examples-dir> <allow.json>

The manifest is generated from the flake (nix/studio-manifest.nix), so it cannot
be edited into agreement with anything; what it can do is quietly stop covering
something. A new pc-* build written without mkGame, a jump ROM whose base was
renamed, a new examples/ directory nobody wired up, a package the studio would
list as a mystery — each of those is invisible until someone looks for the game
in the hub and it is not there. This check turns each into a named failure.

It proves it fires. Before trusting the real manifest it runs the same rules on
copies it has broken on purpose — a pc build stripped of its kind, a jump
pointing at a missing base, an examples/ directory with no ROM, an unrecorded
package — and exits non-zero if any break goes unreported.
"""

import copy
import json
import sys
from pathlib import Path


def matches(name, entries):
    return any(name == e or (e.endswith("-") and name.startswith(e)) for e in entries)


def problems(manifest, example_dirs, allow):
    out = []
    pkgs = manifest["packages"]
    names = set(pkgs) | set(manifest["tools"])

    for name in sorted(names):
        rec = pkgs.get(name)
        if name.startswith("pc-") and (rec is None or rec.get("kind") != "pc"):
            out.append(f"{name}: a pc- package without a pc record (built without hostNative.mkGame?)")
        if name.startswith("web-") and (rec is None or rec.get("kind") != "web"):
            out.append(f"{name}: a web- package without a web record (built without hostWasm.mkGame?)")
        if rec is None and not matches(name, allow["tools"]):
            out.append(f"{name}: no kiln record and not an allowlisted tool "
                       f"(tools/studio/manifest_allow.json)")

    for name, rec in sorted(pkgs.items()):
        if rec.get("kind") == "jump":
            base = rec.get("base")
            if base not in pkgs or pkgs[base].get("kind") != "rom":
                out.append(f"{name}: jump into '{base}', which is not a ROM package")
        if rec.get("kind") in ("pc", "web") and rec.get("example") not in manifest["games"]:
            out.append(f"{name}: host build of '{rec.get('example')}', which has no game entry")

    for ex in sorted(example_dirs):
        game = manifest["games"].get(ex)
        if (game is None or not game["roms"]) and ex not in allow["examples_without_rom"]:
            out.append(f"examples/{ex}: no ROM package builds it")

    for check in manifest.get("cheap", []):
        if check not in manifest["checks"]:
            out.append(f"cheap check '{check}' is not a flake check")

    return out


def mutations(manifest, example_dirs):
    """(label, manifest, example_dirs) — each must produce at least one problem."""
    pc = next(n for n, r in manifest["packages"].items() if r["kind"] == "pc")
    jump = next(n for n, r in manifest["packages"].items() if r["kind"] == "jump")

    m1 = copy.deepcopy(manifest)
    del m1["packages"][pc]
    m1["tools"].append(pc)

    m2 = copy.deepcopy(manifest)
    m2["packages"][jump]["base"] = "no-such-rom"

    m3 = copy.deepcopy(manifest)
    m3["tools"].append("mystery-package")

    m4 = copy.deepcopy(manifest)
    m4["cheap"] = list(m4.get("cheap", [])) + ["no-such-check"]

    return [
        (f"{pc} loses its pc record", m1, example_dirs),
        (f"{jump} points at a missing base", m2, example_dirs),
        ("an unrecorded, unlisted package appears", m3, example_dirs),
        ("examples/unwired has no ROM", manifest, set(example_dirs) | {"unwired"}),
        ("a cheap check that does not exist", m4, example_dirs),
    ]


def main(argv):
    if len(argv) != 4:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    manifest = json.loads(Path(argv[1]).read_text())
    example_dirs = {p.name for p in Path(argv[2]).iterdir() if p.is_dir()}
    allow = json.loads(Path(argv[3]).read_text())

    fail = 0
    baseline = set(problems(manifest, example_dirs, allow))
    print("── the check fires on broken manifests ──")
    for label, m, ex in mutations(manifest, example_dirs):
        # Only what the break ADDS counts: a rule that cannot fire must not pass
        # on the strength of a problem the real manifest already has.
        new = [p for p in problems(m, ex, allow) if p not in baseline]
        ok = bool(new)
        print(f"  {'ok  ' if ok else 'FAIL'} {label}: "
              + (f"{len(new)} new — {new[0]}" if new else "NOT DETECTED"))
        fail += 0 if ok else 1

    print("── the real manifest ──")
    real = problems(manifest, example_dirs, allow)
    for p in real:
        print(f"  FAIL {p}")
    fail += len(real)

    games = manifest["games"]
    print(f"  {len(games)} games, {len(manifest['packages'])} recorded packages, "
          f"{len(manifest['tools'])} tools, {len(manifest['checks'])} checks, "
          f"{sum(1 for g in games.values() if g['hostable'])} hostable")
    print("studio-manifest: " + ("FAILED" if fail else "ok"))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
