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

sys.path.insert(0, str(BLENDER_SCRIPTS))
import quake_map  # noqa: E402 — pure-Python half only; this process has no bpy
import godot_scene  # noqa: E402

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

    categories = {
        "enemies": [], "heavies": [], "health": [], "armor": [],
        "ammo": [], "npcs": [], "chests": [], "key_doors": [],
        "key_red": [], "switches": [], "barrels": [], "triggers": [],
        "player_start": [], "other": [],
    }

    for ent in entities:
        if ent["brushes"]:
            continue
        props = ent["props"]
        cn = props.get("classname", "?")
        origin = props.get("origin", "?")
        angle = props.get("angle", "0")
        epairs = {k: v for k, v in props.items()
                   if k not in ("classname", "origin", "angle")}

        entry = {"classname": cn, "origin": origin, "angle": angle,
                  "epairs": epairs}

        if cn == "info_player_start":
            categories["player_start"].append(entry)
        elif cn == "info_enemy":
            categories["enemies"].append(entry)
        elif cn == "info_heavy":
            categories["heavies"].append(entry)
        elif cn == "info_health":
            categories["health"].append(entry)
        elif cn == "info_armor":
            categories["armor"].append(entry)
        elif cn == "info_ammo":
            categories["ammo"].append(entry)
        elif cn == "info_npc":
            categories["npcs"].append(entry)
        elif cn == "info_chest":
            categories["chests"].append(entry)
        elif cn == "info_key_door":
            categories["key_doors"].append(entry)
        elif cn == "info_key_red":
            categories["key_red"].append(entry)
        elif cn == "info_switch":
            categories["switches"].append(entry)
        elif cn == "info_barrel":
            categories["barrels"].append(entry)
        elif cn == "info_trigger":
            categories["triggers"].append(entry)
        else:
            categories["other"].append(entry)

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

    for trig in categories["triggers"]:
        for req in ("mins", "maxs", "event_id", "type"):
            if req not in trig["epairs"]:
                issues.append(f"trigger at {trig['origin']} missing '{req}' epair")

    for npc in categories["npcs"]:
        if "dialogue" not in npc["epairs"]:
            issues.append(f"npc at {npc['origin']} missing 'dialogue' epair")

    for chest in categories["chests"]:
        if "contents" not in chest["epairs"]:
            issues.append(f"chest at {chest['origin']} missing 'contents' epair")

    summary = {
        "player_start": len(categories["player_start"]),
        "enemies": len(categories["enemies"]),
        "heavies": len(categories["heavies"]),
        "health_pickups": len(categories["health"]),
        "armor_pickups": len(categories["armor"]),
        "ammo_pickups": len(categories["ammo"]),
        "npcs": len(categories["npcs"]),
        "chests": len(categories["chests"]),
        "key_doors": len(categories["key_doors"]),
        "key_red_pickups": len(categories["key_red"]),
        "switches": len(categories["switches"]),
        "barrels": len(categories["barrels"]),
        "triggers": len(categories["triggers"]),
        "other": len(categories["other"]),
    }

    return {"categories": categories, "summary": summary, "issues": issues}


if __name__ == "__main__":
    mcp.run()
