# SPDX-License-Identifier: MIT
"""The validators Kiln Studio can run, and how each one's output becomes a report.

Every entry runs a tool that already exists — the map validator, camlint, the
gait measurer, the poser's convention check — as a job, and turns what it prints
into tools/schema/report.schema.json. Tools that already speak the schema (with
--json) pass through; the map validator has its own long-standing JSON shape,
which `adapt_map` translates rather than changing a format other things read.

A validator's argument is chosen from a list the studio computes (the .map files
under assets/, the shot files it knows), never typed: the job's argv is built
from the entry, and an argument that is not on the list is refused.
"""

from pathlib import Path


def _rel(repo, paths):
    # A symlink is never offered: `assets/x.map -> /somewhere/else` would have a
    # validator read, and print, a file outside the repository.
    return sorted(str(Path(p).relative_to(repo)) for p in paths if not Path(p).is_symlink())


def adapt_map(native):
    """tools/mapmaker/validate.py --json (mapfmt.analyse) -> a report."""
    if not isinstance(native, dict):
        return None
    errors, notes = [], []
    if native.get("parse_error"):
        errors.append({"code": "PARSE", "msg": str(native["parse_error"])})
    for d in native.get("degenerate") or []:
        errors.append({"code": "DEGENERATE_BRUSH", "where": f"brush {d.get('brush')}",
                       "msg": f"brush {d.get('brush')}: {d.get('cause', 'degenerate')}"
                              + (f" — {d['detail']}" if d.get("detail") else "")})
    for d in native.get("bad_coord") or []:
        errors.append({"code": "BAD_COORD", "where": f"brush {d.get('brush')}",
                       "msg": f"brush {d.get('brush')} has a coordinate outside the engine's range"})
    for d in native.get("over_limit") or []:
        errors.append({"code": "OVER_LIMIT", "where": f"brush {d.get('brush')}",
                       "msg": f"brush {d.get('brush')}: {d.get('planes')} planes, "
                              f"{d.get('max_face_verts')} verts on a face — over the parser's limits"})
    for d in native.get("bad_origin") or []:
        errors.append({"code": "BAD_ORIGIN", "where": f"spawn {d.get('spawn')}",
                       "msg": f"{d.get('classname')} origin {d.get('origin')} does not parse"})
    for d in native.get("bad_angle") or []:
        errors.append({"code": "BAD_ANGLE", "where": f"spawn {d.get('spawn', '?')}",
                       "msg": f"bad angle: {d}"})
    for p in native.get("problems") or []:
        text = p if isinstance(p, str) else str(p)
        if not any(text in e["msg"] for e in errors):
            errors.append({"code": "MAP", "msg": text})
    for w in native.get("fps_warnings") or []:
        notes.append({"code": "FPS", "msg": w if isinstance(w, str) else str(w)})
    if native.get("exit") and not errors:
        errors.append({"code": "MAP", "msg": f"validator exited {native['exit']}"})
    metrics = {k: native[k] for k in ("entities", "brushes", "faces", "spawns", "classnames", "limits")
               if k in native}
    return {"tool": "map-validate", "version": 1, "ok": not errors, "errors": errors, "notes": notes,
            "metrics": metrics}


def registry(repo, python, nix):
    """{id: spec}. spec: label, args() -> allowed arguments, steps(arg) -> [argv, ...]
    ("{out}" in a later step is the previous step's first output path),
    adapt(parsed JSON) -> report or None when the tool already emits one."""
    repo = Path(repo)
    flake = str(repo)
    build = [nix, "build", "--no-link", "--print-out-paths", "--log-format", "internal-json", "-L"]
    return {
        "map-validate": {
            "label": "Level validator (.map CSG, limits, spawns)",
            "args": lambda: _rel(repo, (repo / "assets").glob("*.map")),
            "steps": lambda arg: [[python, str(repo / "tools/mapmaker/validate.py"), str(repo / arg), "--json"]],
            "adapt": adapt_map,
        },
        "camlint": {
            "label": "Camera shot lint (kiln_camlint)",
            "args": lambda: _rel(repo, list((repo / "tools/camlint/fixtures").glob("*.shot.json"))
                                 + list((repo / "assets").glob("**/*.shot.json"))),
            "steps": lambda arg: [build + [f"{flake}#camlint"], ["{out}/bin/camlint", str(repo / arg), "--json"]],
            "adapt": None,
        },
        "gait": {
            "label": "Goblin gait (ground speed, clearance)",
            "args": lambda: ["goblin"],
            "steps": lambda arg: [build + [f"{flake}#model-goblin"],
                                  [python, str(repo / "tools/blender/gait.py"), "{out}/share/gltf/goblin.gltf", "--json"]],
            "adapt": None,
        },
        "poser-verify": {
            "label": "Poser Euler convention vs Blender's export",
            "args": lambda: ["goblin", "dank"],
            "steps": lambda arg: [[python, str(repo / "tools/poser/verify.py"), arg, "--json"]],
            "adapt": None,
        },
    }
