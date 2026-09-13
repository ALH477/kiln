#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""tools/blender-mcp/server.py — MCP server for authoring Kiln level geometry
from Quake .map files and Godot .tscn scenes.

Not related to, and shares no code with, the third-party `ahujasid/blender-
mcp` GitHub project (a Blender addon + in-Blender socket server). This file
is written from scratch against the official `mcp`/FastMCP SDK and drives
Blender headlessly via subprocess — there is no addon and nothing runs
inside Blender. Do not add that project as a dependency here; it is not
one, and the name collision is coincidental.

This is the conversational front-end to tools/blender/quake_map.py and
tools/blender/godot_scene.py — the actual importers, which this server does
not reimplement, only drives. See those two files' module docstrings for
what each format's importer does and does not support before assuming a
map/scene "didn't work" is a bug here rather than an unsupported feature
(Valve 220 .map format, Godot inline sub_resource meshes, and non-mesh nodes
of any kind are all out of scope by design, not oversights).

── Why this runs OUTSIDE the Nix build ─────────────────────────────────────
Every other asset pipeline in this repo (nix/assets.nix, nix/blender.nix) is
hermetic: given the same inputs, `nix build` reproduces the same output byte
for byte, and CI can verify that without a human. A .map or .tscn a user is
actively iterating on is the opposite case — arbitrary, not-yet-committed
content, being converted over and over while someone looks at the numbers
and decides whether the import is even right. Shelling out to the SAME
`blender`/`f3d_inject.py`/`gltf_to_t3d` tools directly (no Nix derivation
per call) is what makes that loop fast. Once a map is finished, committing
it under `assets/` and wiring `nix/blender.nix`'s `mkQuakeMapModel` /
`mkGodotSceneModel` (see import_*'s `nix_snippet` field) gets it onto the
hermetic path — the exact same two functions this server's import tools call
underneath, so nothing about the conversion changes when it moves.

── Requirements ─────────────────────────────────────────────────────────────
`blender` on PATH (or $BLENDER). `gltf_to_t3d`/`mkasset` are optional — found
via $N64_INST/bin (set by `nix develop`) if present; without them, import
tools stop at the glTF stage and say so, rather than fail.

── Running it ───────────────────────────────────────────────────────────────
    nix run .#blender-mcp

That resolves the `mcp` package through THIS flake's own pinned nixpkgs
(flake.lock) — not `nix shell --impure --expr 'import <nixpkgs> {}'`, which
would resolve against whatever channel the caller's NIX_PATH happens to
point at, unpinned. See flake.nix's `apps.blender-mcp` for the wrapper.

Claude Code MCP config (~/.claude/mcp.json or project .mcp.json):
    {
      "mcpServers": {
        "kiln-blender": {
          "command": "nix",
          "args": ["run", "/absolute/path/to/Kiln#blender-mcp"]
        }
      }
    }
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from mcp.server.fastmcp import FastMCP

REPO_ROOT = Path(__file__).resolve().parents[2]
BLENDER_SCRIPTS = REPO_ROOT / "tools" / "blender"
F3D_INJECT = REPO_ROOT / "tools" / "f3d_inject.py"

MAPMAKER = REPO_ROOT / "tools" / "mapmaker"
SCHEMA = REPO_ROOT / "tools" / "schema"

sys.path.insert(0, str(BLENDER_SCRIPTS))
sys.path.insert(0, str(MAPMAKER))
sys.path.insert(0, str(SCHEMA))
import quake_map  # noqa: E402 — pure-Python half only; this process has no bpy
import godot_scene  # noqa: E402
# The .map dialect, the engine's limits and the entity rules, from the one
# place that states them. Importing it rather than restating it is the whole
# point: inspect_fps_map below used to carry its own copy of the epair rules.
import mapfmt  # noqa: E402
import level_vocab  # noqa: E402

mcp = FastMCP("kiln-blender")


def _blender_bin():
    return os.environ.get("BLENDER", shutil.which("blender") or "blender")


def _gltf_to_t3d_bin():
    """None if unavailable — callers degrade to a glTF-only result rather
    than fail, since gltf_to_t3d is genuinely optional for this server
    (only needed to preview the FINAL console-side asset, not to author)."""
    n64_inst = os.environ.get("N64_INST")
    if n64_inst:
        candidate = Path(n64_inst) / "bin" / "gltf_to_t3d"
        if candidate.exists():
            return str(candidate)
    found = shutil.which("gltf_to_t3d")
    return found


def _run_blender(script, script_args, out_gltf, timeout=120):
    cmd = [_blender_bin(), "--background", "--factory-startup", "-noaudio",
           "--python", str(BLENDER_SCRIPTS / script),
           "--", *script_args, "--out", str(out_gltf)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return proc


def _run_f3d_inject(in_gltf, out_gltf, materials, timeout=120):
    args = [sys.executable, str(F3D_INJECT), str(in_gltf), str(out_gltf)]
    for spec in materials:
        args += ["--material", spec]
    proc = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    return proc


def _maybe_build_t3dm(staged_gltf_dir, name, work_dir, base_scale, bvh):
    """Best-effort .t3dm build for a quick preview. Returns (path|None, log
    lines) — None (with an explanatory line) rather than raising, since this
    step is a convenience on top of the real answer (the glTF), not the
    thing being verified."""
    gltf_to_t3d = _gltf_to_t3d_bin()
    if gltf_to_t3d is None:
        return None, ["gltf_to_t3d not found (set $N64_INST or run inside "
                      "`nix develop`) — stopping at the glTF; the geometry "
                      "summary above is still accurate."]
    t3dm = work_dir / f"{name}.t3dm"
    cmd = [gltf_to_t3d, str(staged_gltf_dir / f"{name}.gltf"), str(t3dm),
           f"--base-scale={base_scale}", f"--asset-path={staged_gltf_dir}/",
           "--verbose"] + (["--bvh"] if bvh else [])
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=staged_gltf_dir,
                           timeout=120)
    log = proc.stdout.splitlines() + proc.stderr.splitlines()
    if proc.returncode != 0 or not t3dm.exists():
        return None, log + [f"gltf_to_t3d exited {proc.returncode}"]
    return t3dm, log


def _nix_quake_snippet(name, map_path):
    return (
        f'blenderLib.mkQuakeMapModel {{\n'
        f'  name = "{name}";\n'
        f'  src = {_repo_relative_nix_path(map_path)};\n'
        f'}}'
    )


def _nix_godot_snippet(name, project_root, scene_path):
    return (
        f'blenderLib.mkGodotSceneModel {{\n'
        f'  name = "{name}";\n'
        f'  src = {_repo_relative_nix_path(project_root)};\n'
        f'  scenePath = "{scene_path}";\n'
        f'}}'
    )


def _repo_relative_nix_path(path):
    p = Path(path).resolve()
    try:
        rel = p.relative_to(REPO_ROOT)
        return f"./{rel}"
    except ValueError:
        return f'"{p}"  # OUTSIDE the repo — copy it under assets/ first, a bare absolute path is not reproducible'


# ── tools ────────────────────────────────────────────────────────────────

@mcp.tool()
def inspect_quake_map(path: str) -> dict:
    """Parse a Quake .map (no Blender needed) and report entity/brush/plane
    counts, the texture names used, and any point entities (lights, spawns,
    items) that will be skipped by import — so a map can be sanity-checked
    before paying for a full import."""
    text = Path(path).read_text()
    summary = quake_map.inspect_map(text)
    summary["format_note"] = ("standard-format planes only; Valve 220's "
                              "bracketed UV axes are not supported — see "
                              "tools/blender/quake_map.py")
    return summary


@mcp.tool()
def inspect_godot_scene(path: str) -> dict:
    """Parse a Godot .tscn (no Blender needed) and report its node tree:
    which nodes reference an importable mesh (.glb/.gltf/.obj), which
    reference something unsupported, and which are non-mesh nodes (lights,
    collision shapes, markers) that import will skip."""
    text = Path(path).read_text()
    return godot_scene.inspect_scene(text)


@mcp.tool()
def import_quake_map(path: str, name: str, out_dir: str,
                     scale: float = None, build_preview_t3dm: bool = True,
                     base_scale: int = 64) -> dict:
    """Import a Quake .map's brush geometry into a glTF at
    <out_dir>/<name>.gltf (materials tagged via f3d_inject's `shade` preset
    — untextured, vertex-lit; see tools/f3d_inject.py for other presets if a
    specific texture set matters). If gltf_to_t3d is available
    ($N64_INST/bin, i.e. inside `nix develop`), also builds a preview
    <name>.t3dm and reports its size.

    Returns geometry counts, output paths, a `nix_snippet` for wiring the
    finished map into flake.nix via mkQuakeMapModel once it's checked into
    assets/, and blender/gltf_to_t3d's stdout/stderr for debugging a bad
    import.
    """
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    map_path = Path(path)

    text = map_path.read_text()
    quick = quake_map.inspect_map(text)  # fail fast on a syntax error before spending a Blender launch

    raw_gltf = out_dir / f"{name}.raw.gltf"
    args = ["--map", str(map_path)]
    if scale is not None:
        args += ["--scale", str(scale)]
    proc = _run_blender("quake_map.py", args, raw_gltf)

    result = {
        "entities": quick["entities"], "brushes": quick["brushes"],
        "point_entities": quick["point_entities"],
        "blender_returncode": proc.returncode,
        "blender_log": (proc.stdout + proc.stderr).splitlines()[-40:],
    }
    if proc.returncode != 0 or not raw_gltf.exists():
        result["error"] = "Blender import failed — see blender_log"
        return result

    tagged_gltf = out_dir / f"{name}.gltf"
    f3d_proc = _run_f3d_inject(raw_gltf, tagged_gltf, ["*=shade"])
    result["f3d_inject_log"] = (f3d_proc.stdout + f3d_proc.stderr).splitlines()
    if f3d_proc.returncode != 0:
        result["error"] = "f3d_inject failed — see f3d_inject_log"
        return result
    shutil.copy(out_dir / f"{name}.raw.bin", out_dir / f"{name}.bin")

    result["gltf_path"] = str(tagged_gltf)
    result["nix_snippet"] = _nix_quake_snippet(name, map_path)

    if build_preview_t3dm:
        t3dm, log = _maybe_build_t3dm(out_dir, name, out_dir, base_scale, bvh=True)
        result["t3dm_path"] = str(t3dm) if t3dm else None
        result["t3dm_log"] = log
        if t3dm:
            result["t3dm_bytes"] = t3dm.stat().st_size

    return result


@mcp.tool()
def import_godot_scene(path: str, project_root: str, name: str, out_dir: str,
                       scale: float = None, build_preview_t3dm: bool = True,
                       base_scale: int = 64) -> dict:
    """Import a Godot .tscn's mesh-bearing nodes into a glTF at
    <out_dir>/<name>.gltf, each placed per its Transform3D (see
    tools/blender/godot_scene.py's "Axes" note for the Godot->Blender
    conversion). `project_root` resolves the scene's res:// resource paths.

    Returns the same shape as import_quake_map, plus `skipped` (nodes whose
    resource couldn't be imported) and `other_nodes` (non-mesh nodes, for a
    game to turn into KilnActor spawns / lights / rooms by hand)."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    scene_path = Path(path)

    text = scene_path.read_text()
    quick = godot_scene.inspect_scene(text)

    raw_gltf = out_dir / f"{name}.raw.gltf"
    args = ["--scene", str(scene_path), "--project", str(project_root)]
    if scale is not None:
        args += ["--scale", str(scale)]
    proc = _run_blender("godot_scene.py", args, raw_gltf)

    result = {
        "mesh_nodes": quick["mesh_nodes"],
        "unsupported_resource_nodes": quick["unsupported_resource_nodes"],
        "other_nodes": quick["other_nodes"],
        "blender_returncode": proc.returncode,
        "blender_log": (proc.stdout + proc.stderr).splitlines()[-40:],
    }
    if proc.returncode != 0 or not raw_gltf.exists():
        result["error"] = "Blender import failed — see blender_log"
        return result

    tagged_gltf = out_dir / f"{name}.gltf"
    f3d_proc = _run_f3d_inject(raw_gltf, tagged_gltf, ["*=shade"])
    result["f3d_inject_log"] = (f3d_proc.stdout + f3d_proc.stderr).splitlines()
    if f3d_proc.returncode != 0:
        result["error"] = "f3d_inject failed — see f3d_inject_log"
        return result
    shutil.copy(out_dir / f"{name}.raw.bin", out_dir / f"{name}.bin")

    try:
        rel_scene = scene_path.resolve().relative_to(Path(project_root).resolve())
    except ValueError:
        rel_scene = scene_path.name
    result["gltf_path"] = str(tagged_gltf)
    result["nix_snippet"] = _nix_godot_snippet(name, project_root, rel_scene)

    if build_preview_t3dm:
        t3dm, log = _maybe_build_t3dm(out_dir, name, out_dir, base_scale, bvh=True)
        result["t3dm_path"] = str(t3dm) if t3dm else None
        result["t3dm_log"] = log
        if t3dm:
            result["t3dm_bytes"] = t3dm.stat().st_size

    return result


@mcp.tool()
def inspect_fps_map(path: str) -> dict:
    """Parse a Quake .map and report FPS-specific entity categorization,
    epair validation, and cross-references. No Blender needed.

    Returns a structured summary of every FPS entity in the map, grouped
    by type (enemies, pickups, NPCs, doors, switches, triggers, barrels),
    with their epairs and any issues (missing required epairs, broken
    switch→door references, doors without matching keys).
    """
    text = Path(path).read_text()
    entities = quake_map.parse_map(text)

    # Categories and the epair rules come from tools/schema/level_vocab.json.
    # This used to be a 14-way elif chain plus a restatement of
    # tools/mapmaker/validate.py's required_epairs -- a straight duplicate with
    # no shared source, so a classname added to the editor was silently
    # uncategorised here and a new required epair was silently unchecked.
    categories = {c: [] for c in
                  sorted({level_vocab.category_of(n)
                          for n in level_vocab.classnames()} | {"other"})}

    for ent in entities:
        if ent["brushes"]:
            continue
        props = ent["props"]
        cn = props.get("classname", "?")
        entry = {"classname": cn,
                 "origin": props.get("origin", "?"),
                 "angle": props.get("angle", "0"),
                 "epairs": {k: v for k, v in props.items()
                            if k not in ("classname", "origin", "angle")}}
        categories[level_vocab.category_of(cn)].append(entry)

    # Cross-reference validation.
    issues = []
    door_count = len(categories["key_doors"])
    for sw in categories["switches"]:
        td = sw["epairs"].get("target_door", "")
        if td and td != "0":
            try:
                idx = int(td)
                if idx < 0 or idx >= door_count:
                    issues.append(f"switch at {sw['origin']} targets door "
                                   f"index {idx} but only {door_count} door(s) exist")
            except ValueError:
                issues.append(f"switch at {sw['origin']} has unparseable "
                               f"target_door '{td}'")
        elif not td:
            issues.append(f"switch at {sw['origin']} missing 'target_door' epair")

    for door in categories["key_doors"]:
        kid = door["epairs"].get("key_id", "")
        if not kid:
            issues.append(f"key_door at {door['origin']} missing 'key_id' epair")
        elif not categories["key_red"]:
            issues.append(f"key_door at {door['origin']} requires key_id "
                           f"{kid} but no info_key_red pickup exists")

    # The remaining required-epair checks, from the schema rather than three
    # more hand-written loops. info_switch and info_key_door keep their own
    # loops above because they also cross-reference, which is logic, not data.
    _crossref = {"info_switch", "info_key_door"}
    for cn, reqs in level_vocab.required_epairs().items():
        if cn in _crossref:
            continue
        for e in categories.get(level_vocab.category_of(cn), []):
            if e["classname"] != cn:
                continue
            for req in reqs:
                if req not in e["epairs"]:
                    issues.append(
                        f"{cn} at {e['origin']} missing '{req}' epair")

    # Built from the categories dict rather than naming each one: a classname
    # added to the schema now shows up here without an edit.
    summary = {cat: len(items) for cat, items in sorted(categories.items())}
    summary["total_entities"] = sum(len(v) for v in categories.values())

    return {"categories": categories, "summary": summary, "issues": issues}


# ── the headless authoring loop ──────────────────────────────────────────
# emit/validate import mapfmt directly (no subprocess): this process already
# imports quake_map the same way. render must shell out, because the renderer
# is a compiled host binary — the same one nix/checks/kiln-map.nix compiles, so
# what comes back is what the gate compares.

@mcp.tool()
def emit_quake_map(spec: dict, out_path: str) -> dict:
    """Author a Quake .map from a JSON spec and write it to out_path.

    spec is the browser editor's own state shape:
      {"brushes": [{"mins": [x,y,z], "maxs": [x,y,z], "texture": "TEX"}],
       "spawns":  [{"classname": "...", "origin": [x,y,z], "angle": 0,
                    "epairs": {"key": "value"}}]}

    The emitted map is validated before it is written, so this never produces
    a file that validate_quake_map would reject. Use this rather than writing
    .map text by hand: a brush wound the wrong way loads fine on console and
    yields ZERO geometry through the CSG, which is how six of the seven
    committed maps in this repo ended up broken."""
    for i, b in enumerate(spec.get("brushes", [])):
        mn, mx = b.get("mins"), b.get("maxs")
        if not (isinstance(mn, list) and isinstance(mx, list)
                and len(mn) == 3 and len(mx) == 3):
            return {"ok": False,
                    "error": f"brush #{i} needs mins and maxs as 3-element lists"}
        for a in range(3):
            if mn[a] >= mx[a]:
                return {"ok": False,
                        "error": f"brush #{i} is degenerate on {'xyz'[a]}: "
                                 f"{mn[a]} >= {mx[a]}"}
    text = mapfmt.emit_state(spec)
    report = mapfmt.analyse(quake_map.parse_map(text))
    if report["problems"]:
        return {"ok": False, "error": "emitted map does not validate",
                "report": report}
    Path(out_path).write_text(text)
    return {"ok": True, "path": out_path, "brushes": report["brushes"],
            "spawns": report["spawns"], "warnings": report["fps_warnings"]}


@mcp.tool()
def validate_quake_map(path: str) -> dict:
    """Validate a .map: counts against the engine's limits, brushes that do
    not survive the CSG (WITH the cause — inside-out winding, not a closed
    volume, or a duplicated plane), unparseable spawn origins and angles, and
    the FPS entity cross-references.

    A degenerate brush is the failure worth knowing about: it loads on console
    and renders nothing through Blender. `./dev map-canon <file>` repairs it."""
    try:
        entities = quake_map.parse_map(Path(path).read_text())
    except quake_map.MapSyntaxError as e:
        return {"ok": False, "parse_error": str(e)}
    report = mapfmt.analyse(entities)
    report["ok"] = report["exit"] == 0
    return report


@mcp.tool()
def render_quake_map(path: str, out_png: str) -> dict:
    """Draw a .map with the real engine and the real kiln_map.c, to a PNG, with
    no ROM and no emulator. Returns the output path plus the load counts, the
    world AABB and the pixel statistics — then READ the PNG to see it.

    Shells out to `nix run .#map-render`, which compiles the same body
    nix/checks/kiln-map.nix does, so this frame is the one the gate compares."""
    proc = subprocess.run(
        ["nix", "run", ".#map-render", "--", path, out_png],
        cwd=str(REPO_ROOT), capture_output=True, text=True)
    return {
        "ok": proc.returncode == 0,
        "png": out_png if proc.returncode == 0 else None,
        "stdout": proc.stdout.splitlines(),
        "stderr": [ln for ln in proc.stderr.splitlines()
                   if "warning: Git tree" not in ln],
    }


if __name__ == "__main__":
    mcp.run()
