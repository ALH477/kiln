# SPDX-License-Identifier: MIT
"""ow_tile.py — openworld-demo's terrain tiles: four biomes, three LODs each.

    blender --background --factory-startup -noaudio \
        --python tools/blender/ow_tile.py -- --model ow_forest_0 --out build/t

Model names are `ow_<biome>_<lod>`, which is also the StreamDB key the demo
asks for (`models/ow_forest_0.t3dm`), so the name IS the filename — see the
hard-won fact about asset builder names.

── One tile ───────────────────────────────────────────────────────────────
A column of terrain 1.5 Blender units across (96 engine units at the default
base scale of 64) with its top at z = 0 and a skirt down to z = -3.25. The demo
places each tile at its own height, so neighbouring tiles at different heights
read as terraces and the skirt fills the step; 208 units of skirt covers the
largest height the demo's height field makes (192).

── Three LODs that look different, on purpose ────────────────────────────
  0   a 4x4 top with per-corner colour variation, and the biome's props
      (trees, bushes, boulders, a spire, ripples)
  1   a 2x2 top and a reduced prop set
  2   one top quad and the skirt: a convex box, nothing else

LOD 2 being convex is load-bearing. kiln_twopass draws far tiles with the
Z-buffer OFF, and a convex solid with back faces culled (the `shade` preset's
default) draws correctly with no depth test at all. Anything with a prop on
top would not.

Colours are mid-tones: the console renders noticeably darker than the host
build, and a dark floor reads as a hole.
"""

import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import kilnlib as m  # noqa: E402

HALF = 0.75
# The top overhangs the skirt by one engine unit on every side (49/64 BU), so
# neighbouring tiles overlap instead of meeting edge to edge. Two tiles drawn
# through two matrices do not share rasterised edges, and at a shared edge the
# N64 leaves pixel cracks that show the skirt through them as a dark seam.
TOP_HALF = 49.0 / 64.0
SKIRT = -3.25

BIOMES = {
    "water": dict(top=(m.srgb(56, 120, 176), m.srgb(76, 146, 198)),
                  side=(m.srgb(46, 96, 140), m.srgb(30, 64, 96))),
    "grass": dict(top=(m.srgb(104, 170, 76), m.srgb(128, 188, 88)),
                  side=(m.srgb(132, 104, 70), m.srgb(88, 68, 46))),
    "forest": dict(top=(m.srgb(78, 138, 64), m.srgb(96, 154, 72)),
                   side=(m.srgb(116, 92, 62), m.srgb(76, 60, 40))),
    "rock": dict(top=(m.srgb(158, 156, 148), m.srgb(182, 178, 168)),
                 side=(m.srgb(128, 124, 118), m.srgb(92, 90, 86))),
}
# LOD 2's single top quad takes the colour the biome reads as from a distance:
# a forest is its canopy, not the grass under it.
FAR_TOP = {"forest": m.srgb(58, 116, 58)}

CANOPY = (m.srgb(46, 110, 54), m.srgb(84, 150, 70))
TRUNK = m.srgb(110, 78, 48)
BUSH = (m.srgb(62, 128, 58), m.srgb(92, 156, 70))
STONE = (m.srgb(120, 118, 112), m.srgb(170, 166, 156))
SNOW = m.srgb(236, 238, 244)
FOAM = m.srgb(150, 196, 226)


def _hash(*vals):
    h = 2166136261
    for v in vals:
        h = ((h ^ (int(v) & 0xFFFFFFFF)) * 16777619) & 0xFFFFFFFF
    return (h >> 8) / 16777216.0


class Mesh:
    def __init__(self):
        self.verts, self.faces, self.colors = [], [], []

    def poly(self, pts, cols):
        base = len(self.verts)
        self.verts.extend(pts)
        self.colors.extend(cols)
        self.faces.append(tuple(range(base, base + len(pts))))


def top_grid(mesh, n, c0, c1, z=0.0):
    step = 2 * TOP_HALF / n
    for j in range(n):
        for i in range(n):
            x0, y0 = -TOP_HALF + i * step, -TOP_HALF + j * step
            x1, y1 = x0 + step, y0 + step
            corners = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
            cols = [m.mix(c0, c1, _hash(a, b, n)) for a, b in corners]
            mesh.poly([(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)], cols)


def sides(mesh, x0, x1, y0, y1, zb, zt, ct, cb):
    """Four outward-facing walls, counter-clockwise seen from outside."""
    cols = [cb, cb, ct, ct]
    mesh.poly([(x0, y0, zb), (x1, y0, zb), (x1, y0, zt), (x0, y0, zt)], cols)  # -Y
    mesh.poly([(x1, y1, zb), (x0, y1, zb), (x0, y1, zt), (x1, y1, zt)], cols)  # +Y
    mesh.poly([(x1, y0, zb), (x1, y1, zb), (x1, y1, zt), (x1, y0, zt)], cols)  # +X
    mesh.poly([(x0, y1, zb), (x0, y0, zb), (x0, y0, zt), (x0, y1, zt)], cols)  # -X


def cube(mesh, cx, cy, hx, hy, z0, z1, ct, cb):
    sides(mesh, cx - hx, cx + hx, cy - hy, cy + hy, z0, z1, ct, cb)
    mesh.poly([(cx - hx, cy - hy, z1), (cx + hx, cy - hy, z1),
               (cx + hx, cy + hy, z1), (cx - hx, cy + hy, z1)], [ct] * 4)


def cone(mesh, cx, cy, r, z0, h, segs, c_base, c_tip, top_r=0.0, cap=None):
    ring = [(cx + r * math.cos(2 * math.pi * k / segs),
             cy + r * math.sin(2 * math.pi * k / segs), z0) for k in range(segs)]
    if top_r <= 0.0:
        apex = (cx, cy, z0 + h)
        for k in range(segs):
            mesh.poly([ring[k], ring[(k + 1) % segs], apex], [c_base, c_base, c_tip])
        return
    top = [(cx + top_r * math.cos(2 * math.pi * k / segs),
            cy + top_r * math.sin(2 * math.pi * k / segs), z0 + h) for k in range(segs)]
    for k in range(segs):
        k1 = (k + 1) % segs
        mesh.poly([ring[k], ring[k1], top[k1], top[k]], [c_base, c_base, c_tip, c_tip])
    centre = (cx, cy, z0 + h)
    cap = cap or c_tip
    for k in range(segs):
        mesh.poly([centre, top[k], top[(k + 1) % segs]], [cap, cap, cap])


def tree(mesh, x, y, s, segs, trunk=True):
    if trunk:
        cube(mesh, x, y, 0.035 * s, 0.035 * s, 0.0, 0.16 * s, TRUNK, TRUNK)
    cone(mesh, x, y, 0.22 * s, 0.12 * s, 0.62 * s, segs, CANOPY[0], CANOPY[1])


def bush(mesh, x, y, s):
    cube(mesh, x, y, 0.09 * s, 0.09 * s, 0.0, 0.10 * s, BUSH[1], BUSH[0])


def boulder(mesh, x, y, s, segs=5):
    cone(mesh, x, y, 0.20 * s, -0.02, 0.18 * s, segs, STONE[0], STONE[1],
         top_r=0.12 * s)


def props(mesh, biome, lod):
    if lod >= 2:
        return
    if biome == "forest":
        if lod == 0:
            for x, y, s in ((-0.42, -0.36, 1.0), (0.36, -0.42, 0.85),
                            (-0.30, 0.40, 0.95), (0.42, 0.34, 1.10), (0.02, 0.0, 0.75)):
                tree(mesh, x, y, s, 6)
        else:
            for x, y, s in ((-0.28, -0.22, 1.1), (0.30, 0.30, 1.0)):
                tree(mesh, x, y, s, 4, trunk=False)
    elif biome == "grass":
        spots = ((-0.40, 0.30, 1.0), (0.35, -0.25, 1.3), (0.10, 0.45, 0.8)) if lod == 0 \
            else ((0.20, -0.10, 1.4),)
        for x, y, s in spots:
            bush(mesh, x, y, s)
    elif biome == "rock":
        if lod == 0:
            boulder(mesh, -0.36, -0.30, 1.2)
            boulder(mesh, 0.40, 0.36, 0.9)
            cone(mesh, 0.12, -0.22, 0.20, 0.0, 0.80, 5, STONE[0], SNOW)
        else:
            cone(mesh, 0.0, 0.0, 0.26, 0.0, 0.62, 4, STONE[0], SNOW)
    elif biome == "water" and lod == 0:
        for x, y in ((-0.35, -0.2), (0.3, 0.38), (0.25, -0.45)):
            mesh.poly([(x - 0.10, y - 0.02, 0.01), (x + 0.10, y - 0.02, 0.01),
                       (x + 0.10, y + 0.02, 0.01), (x - 0.10, y + 0.02, 0.01)], [FOAM] * 4)


def build(biome, lod):
    spec = BIOMES[biome]
    mesh = Mesh()
    n = (4, 2, 1)[lod]
    top0, top1 = spec["top"]
    if lod == 2 and biome in FAR_TOP:
        top0 = top1 = FAR_TOP[biome]
    sides(mesh, -HALF, HALF, -HALF, HALF, SKIRT, 0.0,
          m.mix(top0, spec["side"][0], 0.6), spec["side"][1])
    top_grid(mesh, n, top0, top1)
    props(mesh, biome, lod)
    m.make_mesh(f"Tile_{biome}_{lod}", mesh.verts, mesh.faces, "TileMat",
                colors=mesh.colors)


def main():
    name = m.arg("--model")
    parts = name.split("_")
    if len(parts) != 3 or parts[0] != "ow" or parts[1] not in BIOMES or parts[2] not in "012":
        raise SystemExit(f"ow_tile.py: no model '{name}'; want ow_<"
                         f"{'|'.join(BIOMES)}>_<0|1|2>")
    m.reset_scene()
    build(parts[1], int(parts[2]))
    m.report(max_tris=(160, 60, 12)[int(parts[2])])
    m.export_gltf(m.arg("--out"))


main()
