# SPDX-License-Identifier: MIT
"""quake_map.py — import a Quake .map (standard/"Valve 220" NOT supported,
see below) into geometry via kilnlib, so it flows through the same
Blender -> f3d_inject -> gltf_to_t3d pipeline every other model here does.

    blender --background --factory-startup -noaudio \
        --python tools/blender/quake_map.py -- --map path/to/level.map \
        --out build/level

── Why brushes, not a mesh importer ────────────────────────────────────────
A Quake .map is not a mesh format. Each brush is a convex solid defined as the
intersection of half-spaces — one per face, each given as three points on its
plane (the "standard" format every editor, including TrenchBroom's default
export, still writes). There is no vertex list to import; the vertices have to
be DERIVED by intersecting planes, same as qbsp does when it compiles a .map
into a .bsp. brush_to_faces() below is that derivation:

  1. every triple of planes that intersects at a single point is a CANDIDATE
     vertex (solve the 3x3 linear system; a singular/near-singular system
     means those three planes don't meet at a point, e.g. two are parallel)
  2. a candidate survives only if it is on the inside (or boundary) of EVERY
     other plane in the brush — an outside point means those three planes
     meet somewhere outside the solid, which happens constantly for any brush
     with more than 4 faces
  3. each surviving vertex belongs to every plane it lies on; group by plane,
     sort into a winding order (angle around the face's own centroid,
     projected into the face's 2D basis), and that ordered ring is the face

Cost is O(planes^3) per brush, which is fine — map brushes are usually 6-20
planes, not thousands.

── Standard format only ────────────────────────────────────────────────────
Valve 220 format adds explicit UV axis vectors in brackets after the texture
name — `[ux uy uz ou] [vx vy vz ov]` — instead of the standard format's
rotation/scale pair. This importer does not parse that (texturing is thrown
away anyway; see "Materials" below), but it also does not TOLERATE the extra
brackets in the plane line — a Valve-220 .map fails to parse here with a
syntax error rather than silently mis-reading UVs. Re-export from the level
editor in "Standard" format (TrenchBroom: File -> Export -> Map, Standard).

── Materials ────────────────────────────────────────────────────────────────
Quake texture names become Blender material names (sanitised — glTF and
Blender both reject some characters .map texture names allow), one per unique
texture in the file. Nothing here reads the actual texture image or UV
projection; every brush face gets planar UVs good enough for f3d_inject's
`shade` preset (vertex-colour x lighting, no texture). Wiring an actual
texture per Quake material through f3d_inject's `tex0_shade` preset is a
per-project follow-up (needs an actual PNG per texture name), not attempted
here — see nix/blender.nix's `materials` parameter to do that when a specific
map's texture set is known.

── What is skipped ─────────────────────────────────────────────────────────
Point entities (lights, spawn points, item pickups — anything without a
brush list) are parsed but NOT turned into geometry or actors; they are only
reported in inspect_map()'s summary. A game wanting them as KilnActor spawns
reads the reported classname/origin list and writes its own room spawn table
(kiln_room.h's KilnRoomSpawn) by hand — this importer's job stops at geometry.
"""

import math
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

EPSILON = 1e-5


# ── parsing (pure Python — no bpy needed, so this half is unit-testable with
#    a plain `python3 -c` one-liner without going through Blender at all) ──

_PLANE_RE = re.compile(
    r"\(\s*([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)\s*\)\s*"
    r"\(\s*([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)\s*\)\s*"
    r"\(\s*([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)\s*\)\s*"
    r"(\S+)\s+([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)\s+([-\d.eE]+)"
)


class MapSyntaxError(SystemExit):
    pass


def parse_map(text):
    """.map text -> list of entities: {"props": {...}, "brushes": [brush, ...]}
    brush: list of planes; plane: {"p1","p2","p3","texture","xoff","yoff",
    "rot","xscale","yscale"} with the three points as (x,y,z) tuples.

    A hand-rolled recursive-descent-by-brace-counting parser rather than a
    grammar library: the format is three nesting levels deep (map { entity {
    brush } }) with no other structure, and pulling in a parser generator for
    that would be a much bigger dependency than the problem.
    """
    entities = []
    i = 0
    n = len(text)
    depth = 0
    entity = None
    brush = None

    def skip_ws_comments(i):
        while i < n:
            if text[i] in " \t\r\n":
                i += 1
            elif text[i] == "/" and i + 1 < n and text[i + 1] == "/":
                while i < n and text[i] != "\n":
                    i += 1
            else:
                break
        return i

    while True:
        i = skip_ws_comments(i)
        if i >= n:
            break
        c = text[i]

        if c == "{":
            depth += 1
            if depth == 1:
                entity = {"props": {}, "brushes": []}
            elif depth == 2:
                brush = []
            elif depth != 3:
                # A brush's planes are flat lines, not their own { } block in
                # the standard format — depth should never reach 4.
                raise MapSyntaxError(f"quake_map: unexpected '{{' at depth {depth}"
                                     f" (byte {i}) — is this Valve 220 format?"
                                     f" (see the module docstring)")
            i += 1

        elif c == "}":
            if depth == 2:
                entity["brushes"].append(brush)
                brush = None
            elif depth == 1:
                entities.append(entity)
                entity = None
            depth -= 1
            if depth < 0:
                raise MapSyntaxError(f"quake_map: unmatched '}}' at byte {i}")
            i += 1

        elif c == '"':
            if depth != 1:
                raise MapSyntaxError(f"quake_map: key/value pair outside an "
                                     f"entity block at byte {i}")
            end = text.index('"', i + 1)
            key = text[i + 1:end]
            i = skip_ws_comments(end + 1)
            if text[i] != '"':
                raise MapSyntaxError(f"quake_map: expected a quoted value for "
                                     f"'{key}' at byte {i}")
            end2 = text.index('"', i + 1)
            value = text[i + 1:end2]
            entity["props"][key] = value
            i = end2 + 1

        elif c == "(":
            if depth != 2:
                raise MapSyntaxError(f"quake_map: plane line outside a brush "
                                     f"block at byte {i}")
            m = _PLANE_RE.match(text, i)
            if not m:
                # Most likely cause in practice: Valve 220's bracketed UV axes
                # instead of the standard rotation/scale pair.
                raise MapSyntaxError(
                    f"quake_map: couldn't parse a plane line at byte {i} "
                    f"(only standard-format planes are supported — Valve 220's "
                    f"[ux uy uz ou] [vx vy vz ov] UV axes are not; re-export "
                    f"as Standard format)")
            g = m.groups()
            plane = {
                "p1": tuple(float(x) for x in g[0:3]),
                "p2": tuple(float(x) for x in g[3:6]),
                "p3": tuple(float(x) for x in g[6:9]),
                "texture": g[9],
                "xoff": float(g[10]), "yoff": float(g[11]),
                "rot": float(g[12]), "xscale": float(g[13]), "yscale": float(g[14]),
            }
            brush.append(plane)
            i = m.end()

        else:
            raise MapSyntaxError(f"quake_map: unexpected character {c!r} at "
                                 f"byte {i}")

    if depth != 0:
        raise MapSyntaxError(f"quake_map: {depth} unclosed '{{' at end of file")
    return entities


# ── brush geometry (also pure Python) ───────────────────────────────────────

def _sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])
def _dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def _len(a): return math.sqrt(_dot(a, a))
def _norm(a):
    l = _len(a)
    if l < EPSILON:
        return None
    return (a[0] / l, a[1] / l, a[2] / l)


def plane_normal_dist(plane):
    """Quake winding: p1,p2,p3 are clockwise as seen from OUTSIDE the solid
    (the point the normal points toward) — verified against the canonical
    6-plane axial cube example (every Quake mapping tutorial reproduces it):
    the p1=(-64,-64,-64) face varies Y then Z from p1, and needs outward
    normal (-1,0,0); only cross(p3-p1, p2-p1) gives that, not the more
    'obvious' cross(p2-p1, p3-p1)."""
    n = _cross(_sub(plane["p3"], plane["p1"]), _sub(plane["p2"], plane["p1"]))
    n = _norm(n)
    if n is None:
        raise MapSyntaxError(f"quake_map: degenerate plane (collinear points) "
                             f"on texture '{plane['texture']}'")
    return n, _dot(n, plane["p1"])


def _intersect3(p1, p2, p3):
    """The single point common to three planes (n.x = d each), or None if the
    3x3 system is singular (two parallel, or all three sharing a common line).

    v = (d1(n2 x n3) + d2(n3 x n1) + d3(n1 x n2)) / (n1 . (n2 x n3))

    the standard three-plane-intersection identity (the same one Quake-family
    map compilers use), not a general Gaussian solve — a brush's planes are
    few enough that closed-form beats pulling in a matrix library Blender's
    Python may not ship."""
    n1, d1 = p1
    n2, d2 = p2
    n3, d3 = p3
    det = _dot(n1, _cross(n2, n3))
    if abs(det) < EPSILON:
        return None
    c23, c31, c12 = _cross(n2, n3), _cross(n3, n1), _cross(n1, n2)
    return tuple(
        (d1 * c23[i] + d2 * c31[i] + d3 * c12[i]) / det for i in range(3)
    )


def brush_to_faces(brush):
    """A brush (list of planes) -> {texture: [ [ (x,y,z), ... ], ... ]}, one
    polygon (already wound CCW as seen from outside) per face that survives —
    a brush can have fewer resulting faces than input planes if a plane never
    contributes an edge (redundant/outside the others' intersection)."""
    planes = [plane_normal_dist(p) for p in brush]
    per_plane_verts = [[] for _ in planes]

    for i in range(len(planes)):
        for j in range(i + 1, len(planes)):
            for k in range(j + 1, len(planes)):
                v = _intersect3(planes[i], planes[j], planes[k])
                if v is None:
                    continue
                inside = True
                for n, d in planes:
                    if _dot(n, v) > d + EPSILON:
                        inside = False
                        break
                if not inside:
                    continue
                for idx in (i, j, k):
                    n, d = planes[idx]
                    if abs(_dot(n, v) - d) < 1e-4:
                        per_plane_verts[idx].append(v)

    faces_by_tex = {}
    for (n, d), verts, src in zip(planes, per_plane_verts, brush):
        ring = _order_ring(verts, n)
        if ring is None or len(ring) < 3:
            continue
        faces_by_tex.setdefault(src["texture"], []).append(ring)
    return faces_by_tex


def _order_ring(verts, normal):
    """Dedupe near-coincident points, then sort the remainder into a winding
    order (CCW looking against `normal`, i.e. from outside the solid — the
    convention kilnlib.make_mesh / Blender face winding both want)."""
    uniq = []
    for v in verts:
        if not any(_len(_sub(v, u)) < 1e-4 for u in uniq):
            uniq.append(v)
    if len(uniq) < 3:
        return None

    centroid = tuple(sum(c) / len(uniq) for c in zip(*uniq))
    # A 2D basis in the face's own plane, to angle-sort by.
    ref = _sub(uniq[0], centroid)
    ref = _norm(ref)
    if ref is None:
        ref = _norm(_cross(normal, (1.0, 0.0, 0.0))) or _norm(_cross(normal, (0.0, 1.0, 0.0)))
    u = ref
    v = _cross(normal, u)

    def angle(p):
        d = _sub(p, centroid)
        return math.atan2(_dot(d, v), _dot(d, u))

    uniq.sort(key=angle)
    return uniq


# ── Blender assembly ────────────────────────────────────────────────────────

def _safe_name(texture):
    """Quake texture names allow characters glTF material names and Blender
    data-block names don't love (e.g. leading '*' for liquids, '/' for
    wad-relative paths in some tools). Collapse anything outside
    [A-Za-z0-9_] rather than pass it through and hit a export-time surprise."""
    cleaned = re.sub(r"[^A-Za-z0-9_]+", "_", texture).strip("_")
    return cleaned or "Unnamed"


SKIP_TEXTURES = {"skip", "clip", "trigger", "hint", "areaportal", "caulk",
                 "nodraw", "origin"}


def import_map(text, scale=1.0 / 32.0):
    """Parse + build every worldspawn/brush-entity solid into Blender meshes,
    one object per texture per entity (matching kilnlib's one-material-per-mesh
    convention). Returns a summary dict for the caller to report/log.

    scale: Quake's grid is traditionally ~32 units per Blender-unit-ish human
    scale (a player is 56 units tall); the default brings a typical map into
    the same order of magnitude as this repo's hand-authored models (goblin
    is ~2.3 Blender units tall). Override for a specific map's grid.
    """
    import kilnlib as m

    entities = parse_map(text)

    brush_count = 0
    face_count = 0
    tri_count = 0
    point_entities = []
    verts_by_tex = {}   # texture -> [(x,y,z), ...]
    faces_by_tex = {}   # texture -> [(i,j,k[,l]), ...]

    for ent in entities:
        if not ent["brushes"]:
            props = ent["props"]
            point_entities.append({
                "classname": props.get("classname", "?"),
                "origin": props.get("origin", "0 0 0"),
            })
            continue

        for brush in ent["brushes"]:
            brush_count += 1
            for texture, polys in brush_to_faces(brush).items():
                if texture.lower() in SKIP_TEXTURES:
                    continue
                vlist = verts_by_tex.setdefault(texture, [])
                flist = faces_by_tex.setdefault(texture, [])
                for poly in polys:
                    face_count += 1
                    tri_count += len(poly) - 2
                    base = len(vlist)
                    # Quake Z-up, same handedness as Blender — only a uniform
                    # scale is needed, no axis swap (unlike the glTF/engine
                    # Y-up conversion, which kilnlib's export_gltf handles).
                    vlist.extend((v[0] * scale, v[1] * scale, v[2] * scale)
                                for v in poly)
                    flist.append(tuple(range(base, base + len(poly))))

    for texture in sorted(verts_by_tex):
        mat_name = _safe_name(texture)
        m.make_mesh(f"Brush_{mat_name}", verts_by_tex[texture],
                    faces_by_tex[texture], mat_name,
                    colors=m.srgb(200, 200, 200))

    return {
        "entities": len(entities),
        "brushes": brush_count,
        "faces": face_count,
        "tris": tri_count,
        "materials": sorted(_safe_name(t) for t in verts_by_tex),
        "point_entities": point_entities,
    }


def inspect_map(text):
    """Cheap, bpy-free summary — parse only, no brush CSG — for a caller
    (the MCP server) to sanity-check a file before paying for a full Blender
    import."""
    entities = parse_map(text)
    brush_count = sum(len(e["brushes"]) for e in entities)
    plane_count = sum(len(b) for e in entities for b in e["brushes"])
    textures = sorted({p["texture"] for e in entities for b in e["brushes"]
                       for p in b})
    point_entities = [e["props"].get("classname", "?")
                      for e in entities if not e["brushes"]]
    return {
        "entities": len(entities),
        "brush_entities": sum(1 for e in entities if e["brushes"]),
        "brushes": brush_count,
        "planes": plane_count,
        "textures": textures,
        "point_entities": point_entities,
    }


def main():
    import kilnlib as m

    path = m.arg("--map")
    out = m.arg("--out")
    scale = float(m.arg("--scale", "0.03125"))  # 1/32

    text = Path(path).read_text()
    m.reset_scene()
    summary = import_map(text, scale=scale)

    print(f"  [MAP] {path}: {summary['entities']} entities, "
         f"{summary['brushes']} brushes, {summary['faces']} faces, "
         f"~{summary['tris']} tris, {len(summary['materials'])} materials")
    if summary["point_entities"]:
        print(f"  [MAP] {len(summary['point_entities'])} point entities "
             f"skipped (not geometry) — report these separately if the game "
             f"needs spawns from them")

    m.report()
    m.export_gltf(out)


if __name__ == "__main__" and "bpy" in sys.modules:
    main()
