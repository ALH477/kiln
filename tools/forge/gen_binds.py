#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""gen_binds.py — Forge/src/forge_binds.def -> a Markdown control table.

The bindings used to live in three unrelated places (the in-ROM help strings,
the n64-forge skill, CLAUDE.md), none generated from another, and all three had
drifted -- most visibly into claiming GEO fills with `Z+A`, which the code has
never done. forge_binds.def is the one statement; this puts it in the docs.

    python3 tools/forge/gen_binds.py            # Markdown to stdout
"""
import re
import sys
from pathlib import Path

DEF = Path(__file__).resolve().parents[1].parent / "Forge" / "src" / "forge_binds.def"
MODES = ["GEO", "WALK", "PAINT", "ENT", "LIGHT", "CAM"]


def parse(text=None):
    text = text if text is not None else DEF.read_text()
    out = []
    for m in re.finditer(r'FORGE_BIND\(\s*(\w+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)',
                         text):
        out.append((m.group(1), m.group(2), m.group(3)))
    return out


def markdown(binds=None):
    binds = binds or parse()
    per = {m: [] for m in MODES}
    every = []
    for mode, btn, act in binds:
        (every if mode == "ALL" else per[mode]).append((btn, act))
    lines = ["| mode | control | does |", "|---|---|---|"]
    for m in MODES:
        for i, (btn, act) in enumerate(per[m]):
            lines.append(f"| {m if i == 0 else ''} | `{btn}` | {act} |")
    for i, (btn, act) in enumerate(every):
        lines.append(f"| {'every mode' if i == 0 else ''} | `{btn}` | {act} |")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    sys.stdout.write(markdown())
