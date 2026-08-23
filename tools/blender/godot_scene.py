# SPDX-License-Identifier: MIT
"""godot_scene.py — place a Godot .tscn scene's meshes into Blender, so a
level laid out in the Godot editor flows through the same Blender ->
f3d_inject -> gltf_to_t3d pipeline every other model here does.

    blender --background --factory-startup -noaudio \
        --python tools/blender/godot_scene.py -- \
        --scene path/to/level.tscn --project /path/to/godot/project \
        --out build/level

── What this does and does not do ──────────────────────────────────────────
A .tscn is a scene GRAPH, not a mesh format: it names external resources
(usually .glb/.gltf/.obj a 3D modelling tool produced) and places instances
of them with a per-node Transform3D. This script parses that graph (pure
Python — see parse_tscn below), resolves each mesh-bearing node's resource
path against the Godot project root (Godot's `res://` scheme), imports the
resource with Blender's own glTF/OBJ importer, and applies the node's
transform, converted from Godot's axis convention to Blender's (see
"Axes" below).

It does NOT import Godot's own primitive mesh resources (a `BoxMesh` or
`SphereMesh` defined inline in the .tscn as a `[sub_resource]` rather than
referencing an external file) — only `[ext_resource]` files Blender's own
importers already understand. It does NOT import materials, physics
shapes, lights, or scripts; non-mesh nodes are reported in the summary
(same convention as quake_map.py's point entities) so a game can turn them
into KilnActor spawns or kiln_room lights by hand, reading their type and
origin from the report rather than this script guessing intent.

── Axes: Godot is Y-up, like glTF — NOT like Blender ───────────────────────
Godot's Transform3D uses the same convention glTF does (Y up, -Z forward,
right-handed) — the exact convention `kilnlib.export_gltf`'s `export_yup`
converts BLENDER's Z-up TO on the way out. So a Godot transform needs the
inverse of that conversion on the way IN, which is a straight axis swap
(no reprojection, since it is a signed permutation of orthonormal basis
vectors and therefore exact in floating point):

    Godot (x, y, z)  ->  Blender (x, -z, y)

applied to the transform's origin and to each of its three basis columns
independently — see `_godot_to_blender_transform`. This is the precise
inverse of models.py's own axis table (`_ROT`), not a separate derivation.
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

_HEADER_RE = re.compile(r"^\[(\w+)((?:\s+\w[\w:]*=(?:\"[^\"]*\"|[^\s\]]+))*)\s*\]\s*$")
_ATTR_RE = re.compile(r'(\w[\w:]*)=("(?:[^"\\]|\\.)*"|[^\s]+)')
_TRANSFORM_RE = re.compile(
    r"Transform3D\(\s*([^)]+?)\s*\)")


def _parse_attrs(attr_text):
    attrs = {}
    for m in _ATTR_RE.finditer(attr_text):
        key, raw = m.group(1), m.group(2)
        attrs[key] = raw[1:-1] if raw.startswith('"') else raw
    return attrs


def parse_tscn(text):
    """.tscn text -> {"ext_resources": {id: {"type","path"}}, "nodes": [...]}

    Each node dict: {"name","type","parent","transform"(12 floats or None),
    "mesh_id"(ext_resource id or None), "instance_id"(ext_resource id, for a
    node that IS an instanced external scene, or None)}. `type` is None for
    an `instance=` node — Godot infers its type from the instanced scene,
    which this parser does not open.

    A line-oriented parser, not a general Godot-resource-format parser:
    .tscn is INI-like (`[section key=val ...]` headers, `key = val` body
    lines) specifically, and the only body key read here is `transform` /
    `mesh` / `instance` — everything else in a node body (physics, scripts,
    visibility) is intentionally skipped as text.
    """
    ext_resources = {}
    nodes = []
    section = None
    current = None

    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(";"):
            continue

        header = _HEADER_RE.match(line)
        if header:
            if current is not None:
                nodes.append(current)
                current = None
            section, attr_text = header.group(1), header.group(2)
            attrs = _parse_attrs(attr_text)
            if section == "ext_resource":
                ext_resources[attrs.get("id")] = {
                    "type": attrs.get("type"), "path": attrs.get("path"),
                }
            elif section == "node":
                current = {
                    "name": attrs.get("name", "?"),
                    "type": attrs.get("type"),
                    "parent": attrs.get("parent"),
                    "transform": None,
                    "mesh_id": None,
                    "instance_id": _ext_id(attrs.get("instance")),
                }
            continue

        if section == "node" and current is not None:
            if line.startswith("transform"):
                m = _TRANSFORM_RE.search(line)
                if m:
                    nums = [float(x) for x in m.group(1).split(",")]
                    if len(nums) == 12:
                        current["transform"] = nums
            elif line.startswith("mesh"):
                current["mesh_id"] = _ext_id(line.split("=", 1)[1].strip())

    if current is not None:
        nodes.append(current)

    return {"ext_resources": ext_resources, "nodes": nodes}


def _ext_id(value):
    """`ExtResource("3")` or `ExtResource( "3" )` -> "3", else None."""
    if not value:
        return None
    m = re.match(r'ExtResource\(\s*"?([^")]+)"?\s*\)', value)
    return m.group(1) if m else None


IDENTITY_TRANSFORM = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]


def _godot_to_blender_transform(t):
    """12 Godot Transform3D floats (3 basis columns + origin) -> the same
    shape in Blender space. See the module docstring's "Axes" section —
    (x,y,z) -> (x,-z,y), applied to the origin and to each basis column."""
    def conv(v):
        return (v[0], -v[2], v[1])
    bx, by, bz, origin = t[0:3], t[3:6], t[6:9], t[9:12]
    return conv(bx), conv(by), conv(bz), conv(origin)


def resolve_res_path(path, project_root):
    """`res://models/rock.glb` -> `<project_root>/models/rock.glb`. Any other
    scheme (`user://`, an absolute OS path some importers write) is passed
    through unresolved and will simply fail to open, which is the correct
    behaviour — this importer only understands project-relative assets."""
    if path.startswith("res://"):
        return str(Path(project_root) / path[len("res://"):])
    return path


SUPPORTED_MESH_EXT = {".glb", ".gltf", ".obj"}


def inspect_scene(text):
    """Cheap, bpy-free summary for the MCP server to sanity-check a scene
    before paying for a full Blender import."""
    scene = parse_tscn(text)
    mesh_nodes, other_nodes, unsupported = [], [], []
    for node in scene["nodes"]:
        res_id = node["mesh_id"] or node["instance_id"]
        if res_id and res_id in scene["ext_resources"]:
            path = scene["ext_resources"][res_id]["path"]
            ext = Path(path).suffix.lower()
            if ext in SUPPORTED_MESH_EXT:
                mesh_nodes.append({"name": node["name"], "resource": path})
            else:
                unsupported.append({"name": node["name"], "resource": path})
        elif node["type"] is not None:
            other_nodes.append({"name": node["name"], "type": node["type"]})
    return {
        "nodes": len(scene["nodes"]),
        "ext_resources": len(scene["ext_resources"]),
        "mesh_nodes": mesh_nodes,
        "unsupported_resource_nodes": unsupported,
        "other_nodes": other_nodes,
    }


def import_scene(text, project_root, scale=1.0):
    """Import every supported mesh-bearing node into the current Blender
    scene, transformed per its Transform3D. Returns a summary dict."""
    import bpy
    import kilnlib as m

    scene = parse_tscn(text)
    imported, skipped, other = [], [], []

    for node in scene["nodes"]:
        res_id = node["mesh_id"] or node["instance_id"]
        if not res_id or res_id not in scene["ext_resources"]:
            if node["type"] is not None:
                other.append({"name": node["name"], "type": node["type"]})
            continue

        res = scene["ext_resources"][res_id]
        src = Path(resolve_res_path(res["path"], project_root))
        ext = src.suffix.lower()
        if ext not in SUPPORTED_MESH_EXT or not src.exists():
            skipped.append({"name": node["name"], "resource": res["path"],
                            "reason": "unsupported type" if ext not in SUPPORTED_MESH_EXT
                            else "file not found"})
            continue

        before = set(bpy.context.scene.objects)
        if ext in (".glb", ".gltf"):
            bpy.ops.import_scene.gltf(filepath=str(src))
        else:  # .obj
            bpy.ops.wm.obj_import(filepath=str(src))
        new_objs = [o for o in bpy.context.scene.objects if o not in before]

        transform = node["transform"] or IDENTITY_TRANSFORM
        bx, by, bz, origin = _godot_to_blender_transform(transform)
        # Every object the importer created for this node (glTF can bring in
        # more than one) gets the SAME node transform — Blender has no single
        # "root" to attach it to without also creating an empty per node,
        # which would just be extra objects the exporter has to skip.
        for obj in new_objs:
            if obj.parent is None:  # only the importer's own root(s)
                obj.matrix_world = _matrix_from_columns(bx, by, bz, origin) @ obj.matrix_world
            obj.scale = tuple(s * scale for s in obj.scale)

        for obj in new_objs:
            if obj.type == 'MESH':
                imported.append({"name": node["name"], "object": obj.name,
                                 "resource": res["path"],
                                 "tris": sum(len(p.vertices) - 2 for p in obj.data.polygons)})

    return {"imported": imported, "skipped": skipped, "other_nodes": other}


def _matrix_from_columns(bx, by, bz, origin):
    import mathutils
    return mathutils.Matrix((
        (bx[0], by[0], bz[0], origin[0]),
        (bx[1], by[1], bz[1], origin[1]),
        (bx[2], by[2], bz[2], origin[2]),
        (0.0, 0.0, 0.0, 1.0),
    ))


def main():
    import kilnlib as m

    scene_path = m.arg("--scene")
    project_root = m.arg("--project")
    out = m.arg("--out")
    scale = float(m.arg("--scale", "1.0"))

    text = Path(scene_path).read_text()
    m.reset_scene()
    summary = import_scene(text, project_root, scale=scale)

    print(f"  [GODOT] {scene_path}: {len(summary['imported'])} mesh nodes "
         f"imported, {len(summary['skipped'])} skipped, "
         f"{len(summary['other_nodes'])} non-mesh nodes")
    for s in summary["skipped"]:
        print(f"  [GODOT] skipped '{s['name']}' ({s['resource']}): {s['reason']}")

    m.report()
    m.export_gltf(out)


if __name__ == "__main__" and "bpy" in sys.modules:
    main()
