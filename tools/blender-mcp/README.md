<!-- SPDX-License-Identifier: MIT -->
# Kiln Blender MCP — Quake / Godot map import

**Not related to, and shares no code with, the third-party
[`ahujasid/blender-mcp`](https://github.com/ahujasid/blender-mcp) GitHub
project** (a Blender addon + in-Blender socket server). This server is
written from scratch against the official `mcp`/FastMCP SDK and drives
Blender headlessly via subprocess — there is no addon, and nothing runs
inside a live Blender instance. The name overlap is coincidental; do not
install or depend on that project to use this one.

An MCP server that lets an assistant (or a human, via any MCP client)
convert Quake `.map` levels and Godot `.tscn` scenes into geometry that
flows through this repo's existing Blender → f3d_inject → gltf_to_t3d
pipeline (`nix/blender.nix`). It is an **authoring-time tool** — it runs
outside `nix build`, against arbitrary not-yet-committed content, so an
import can be iterated on interactively. Once a map is finished, it is
meant to be checked into `assets/` and wired into `flake.nix` with
`mkQuakeMapModel`/`mkGodotSceneModel` (every `import_*` tool call reports
the exact Nix snippet to add), putting it on the same hermetic,
twice-built-and-hash-compared path as every hand-authored model.

## What it actually does

The importers are `tools/blender/quake_map.py` and
`tools/blender/godot_scene.py` — read their module docstrings first, they
document precisely what each format supports and what is deliberately out
of scope (Valve 220 `.map` UV format, Godot inline `sub_resource` meshes,
non-mesh nodes of any kind, textures/materials beyond `f3d_inject`'s
`shade` preset). This server does not reimplement any of that; it is a
thin process wrapper: parse (pure Python, no Blender) for the `inspect_*`
tools, then shell out to `blender --background` for the `import_*` tools,
followed by `f3d_inject.py` and — if `gltf_to_t3d` is on hand — a preview
`.t3dm` build.

## Requirements

- `blender` on `$PATH` (or set `$BLENDER`).
- `gltf_to_t3d`/`mkasset` are **optional**. Run inside `nix develop` (which
  sets `$N64_INST`) to get the preview-`.t3dm` step; without it, import
  tools still produce a correct glTF and say so.
- The `mcp` Python package, resolved through this flake's own pinned
  nixpkgs (`flake.lock`) rather than an ad hoc, unpinned `<nixpkgs>` channel
  lookup — run it via:

  ```
  nix run .#blender-mcp
  ```

  (`flake.nix`'s `apps.blender-mcp` wraps `pkgs.python3.withPackages (ps: [
  ps.mcp ])` and execs `tools/blender-mcp/server.py` from the live working
  tree — this server is meant to be run against arbitrary, not-yet-committed
  content, so the script itself is not copied into the Nix store.)

## Wiring into Claude Code

Add to `.mcp.json` (project) or `~/.claude/mcp.json` (user):

```json
{
  "mcpServers": {
    "kiln-blender": {
      "command": "nix",
      "args": ["run", "/absolute/path/to/Kiln#blender-mcp"]
    }
  }
}
```

## Tools

| Tool | Needs Blender? | What it returns |
|---|---|---|
| `inspect_quake_map(path)` | No | entity/brush/plane counts, texture names, point entities |
| `inspect_godot_scene(path)` | No | node tree: mesh nodes, unsupported resources, non-mesh nodes |
| `import_quake_map(path, name, out_dir, ...)` | Yes | geometry counts, `.gltf` path, optional preview `.t3dm`, a `nix_snippet` to wire it in |
| `import_godot_scene(path, project_root, name, out_dir, ...)` | Yes | same, plus which nodes were skipped and why |

`point_entities` (Quake) and `other_nodes` (Godot) are reported, not
imported — spawns, lights, and triggers are content decisions for the game
to make (usually as `kiln_room.h` `KilnRoomSpawn` entries or `KilnActor`
profiles), not something this pipeline should guess at.

## The manual verification this was built against

Before any of this was wired together, the geometry math
(`quake_map.brush_to_faces`, the three-plane intersection formula) was
checked against the canonical 6-plane Quake cube brush with plain
`python3` — no Blender involved — which caught a genuine bug in the
closed-form intersection formula and a winding-order inversion that would
otherwise have shown up as a mangled or inside-out mesh far downstream.
Same for the Godot Y-up → Blender Z-up transform conversion, checked by
importing a scene with two placed cubes and confirming the exported
glTF's node translations matched the source `.tscn`'s `Transform3D`
origins exactly. Reproduce either check by importing
`tools/blender/quake_map.py` / `godot_scene.py` directly with a bare
`python3 -c` — the parsing and geometry functions have no `bpy`
dependency, only `main()` does.
