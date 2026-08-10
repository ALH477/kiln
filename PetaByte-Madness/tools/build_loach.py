#!/usr/bin/env python3
"""
DSV LOACH  -- one-person work submarine, N64-ready low-poly asset.

Universe: Dr. Patrick Horner's underwater lab. Built from salvage by a
competent, exhausted person. Ugly, correct, maintained.

Outputs (into ./out):
  loach.obj / loach.mtl   - modelling / import (vertex colours included)
  loach_hull.png          - 32x32 tiling plate+rust texture
  loach_hull_rgba16.h     - texture as RGBA5551 C array
  loach.h                 - F3DEX2 Vtx array + display lists
  loach_preview.png       - 4-view flat-shaded preview

Orientation: +Z forward (nose), +Y up, +X starboard. 1 unit = 1 metre in OBJ;
exported to the N64 header at 100 units/m (centimetres, s16).
"""

import math, os, random
import numpy as np

random.seed(1977)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
os.makedirs(OUT, exist_ok=True)

# ----------------------------------------------------------------------------
# mesh container
# ----------------------------------------------------------------------------

class Mesh:
    def __init__(self):
        self.verts = []      # (x,y,z)
        self.cols  = []      # (r,g,b)
        self.uvs   = []      # (u,v)
        self.tris  = []      # (i,j,k)
        self.tgrp  = []      # group name per tri
        self._map  = {}

    def v(self, p, col, uv=(0.0, 0.0)):
        key = (round(p[0], 4), round(p[1], 4), round(p[2], 4),
               col, (round(uv[0], 3), round(uv[1], 3)))
        i = self._map.get(key)
        if i is None:
            i = len(self.verts)
            self._map[key] = i
            self.verts.append(tuple(p))
            self.cols.append(col)
            self.uvs.append(uv)
        return i

    def tri(self, a, b, c, g):
        if a == b or b == c or a == c:
            return
        self.tris.append((a, b, c))
        self.tgrp.append(g)

    def quad(self, a, b, c, d, g):
        self.tri(a, b, c, g)
        self.tri(a, c, d, g)


M = Mesh()

# ----------------------------------------------------------------------------
# palette -- drab, oxidised, no styling budget
# ----------------------------------------------------------------------------

HULL      = (146, 128,  74)   # faded safety yellow, gone brown
HULL_RUST = (108,  66,  40)
RIB       = ( 84,  82,  68)
DOME      = ( 44,  76,  78)   # thick scratched acrylic
DOME_HI   = ( 96, 138, 132)
HATCH     = (118,  70,  44)   # red-lead primer, never topcoated
STEEL     = ( 78,  80,  84)
DARK      = ( 48,  50,  54)
BRASS     = (152, 126,  76)
POD       = ( 62,  68,  78)
LENS      = (232, 220, 168)
GRATE     = ( 96,  96,  92)


def shade(col, p, rust=0.0):
    """Bake a crude vertical AO gradient + optional rust bias into the colour."""
    t = 0.58 + 0.42 * min(max((p[1] + 0.75) / 1.45, 0.0), 1.0)
    r, g, b = col
    if rust > 0.0:
        r = r + (HULL_RUST[0] - r) * rust
        g = g + (HULL_RUST[1] - g) * rust
        b = b + (HULL_RUST[2] - b) * rust
    return (int(max(0, min(255, r * t))),
            int(max(0, min(255, g * t))),
            int(max(0, min(255, b * t))))


# ----------------------------------------------------------------------------
# primitives
# ----------------------------------------------------------------------------

def ring_pts(z, r, n, phase=0.0, yflat=None, ysquash=1.0):
    pts = []
    for i in range(n):
        a = phase + 2.0 * math.pi * i / n
        x = math.cos(a) * r
        y = math.sin(a) * r * ysquash
        if yflat is not None:
            y = max(y, yflat)
        pts.append((x, y, z))
    return pts


def lathe(profile, n, group, colfn, uvfn=None, cap_start=False, cap_end=False,
          phase=0.0):
    """profile: list of (z, radius). Builds bands between consecutive rings."""
    rings = []
    for (z, r) in profile:
        rings.append(ring_pts(z, r, n, phase))
    idx = []
    for k, pts in enumerate(rings):
        row = []
        for i, p in enumerate(pts):
            uv = uvfn(i, k, p) if uvfn else (0.0, 0.0)
            row.append(M.v(p, colfn(p, i, k), uv))
        idx.append(row)
    for k in range(len(rings) - 1):
        for i in range(n):
            j = (i + 1) % n
            M.quad(idx[k][i], idx[k][j], idx[k + 1][j], idx[k + 1][i], group)
    if cap_start:
        z, r = profile[0]
        c = M.v((0, 0, z), colfn((0, 0, z), 0, 0), (0.5, 0.5))
        for i in range(n):
            j = (i + 1) % n
            M.tri(c, idx[0][j], idx[0][i], group)
    if cap_end:
        z, r = profile[-1]
        c = M.v((0, 0, z), colfn((0, 0, z), 0, len(rings) - 1), (0.5, 0.5))
        for i in range(n):
            j = (i + 1) % n
            M.tri(c, idx[-1][i], idx[-1][j], group)
    return idx


def box(center, size, group, col, uvscale=1.0):
    cx, cy, cz = center
    sx, sy, sz = (s * 0.5 for s in size)
    P = [(cx - sx, cy - sy, cz - sz), (cx + sx, cy - sy, cz - sz),
         (cx + sx, cy + sy, cz - sz), (cx - sx, cy + sy, cz - sz),
         (cx - sx, cy - sy, cz + sz), (cx + sx, cy - sy, cz + sz),
         (cx + sx, cy + sy, cz + sz), (cx - sx, cy + sy, cz + sz)]
    faces = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
             (2, 3, 7, 6), (1, 2, 6, 5), (0, 4, 7, 3)]
    for f in faces:
        vs = []
        for n_, pi in enumerate(f):
            p = P[pi]
            uv = ((p[0] + p[2]) * uvscale, (p[1] + p[2] * 0.31) * uvscale)
            vs.append(M.v(p, shade(col, p), uv))
        M.quad(vs[0], vs[1], vs[2], vs[3], group)


def plate(p0, p1, p2, p3, group, col, double=False):
    vs = [M.v(p, shade(col, p), ((p[0] + p[2]) * 0.5, p[1] * 0.5))
          for p in (p0, p1, p2, p3)]
    M.quad(vs[0], vs[1], vs[2], vs[3], group)
    if double:
        M.quad(vs[3], vs[2], vs[1], vs[0], group)


def cyl_along(axis, center, r, length, n, group, col, cap_a=True, cap_b=True,
              phase=0.0):
    """axis: 'x','y','z'"""
    cx, cy, cz = center
    h = length * 0.5
    rows = []
    for s in (-h, h):
        pts = []
        for i in range(n):
            a = phase + 2 * math.pi * i / n
            u, w = math.cos(a) * r, math.sin(a) * r
            if axis == 'x':
                p = (cx + s, cy + w, cz + u)
            elif axis == 'y':
                p = (cx + u, cy + s, cz + w)
            else:
                p = (cx + u, cy + w, cz + s)
            pts.append(p)
        rows.append([M.v(p, shade(col, p),
                         (i * 0.35, (0 if s < 0 else 1) * 0.6))
                     for i, p in enumerate(pts)])
    for i in range(n):
        j = (i + 1) % n
        M.quad(rows[0][i], rows[0][j], rows[1][j], rows[1][i], group)
    for which, do in ((0, cap_a), (1, cap_b)):
        if not do:
            continue
        s = -h if which == 0 else h
        if axis == 'x':
            c = (cx + s, cy, cz)
        elif axis == 'y':
            c = (cx, cy + s, cz)
        else:
            c = (cx, cy, cz + s)
        ci = M.v(c, shade(col, c), (0.5, 0.5))
        for i in range(n):
            j = (i + 1) % n
            if which == 0:
                M.tri(ci, rows[0][j], rows[0][i], group)
            else:
                M.tri(ci, rows[1][i], rows[1][j], group)


# ----------------------------------------------------------------------------
# 1. pressure hull -- a repurposed 1,900 L propane tank, ends re-dished
# ----------------------------------------------------------------------------

NSIDE = 10
rustmap = {}


def hull_col(p, i, k):
    key = (i, k)
    if key not in rustmap:
        rustmap[key] = random.random() ** 2.2 * 0.55
    # bottom half collects more growth/rust
    extra = 0.22 if p[1] < -0.15 else 0.0
    return shade(HULL, p, min(0.7, rustmap[key] + extra))


def hull_uv(i, k, p):
    return (i * 0.75, k * 0.9)


hull_profile = [(-1.30, 0.15), (-0.88, 0.47), (0.60, 0.50), (1.00, 0.34)]
lathe(hull_profile, NSIDE, "hull", hull_col, hull_uv, cap_start=True)

# welded stiffener rings (outer band only -- classic N64 collar trick)
for zc in (-0.42, 0.16):
    prof = [(zc - 0.045, 0.545), (zc + 0.045, 0.545)]
    lathe(prof, NSIDE, "ribs", lambda p, i, k: shade(RIB, p, 0.18),
          lambda i, k, p: (i * 0.75, k * 0.35))

# 2. viewport -- 60 mm cast acrylic hemisphere, scratched to hell
lathe([(1.00, 0.34), (1.14, 0.25)], NSIDE, "dome",
      lambda p, i, k: shade(DOME if k == 0 else DOME_HI, p),
      lambda i, k, p: (i * 0.4, k * 0.4), cap_end=True)

# 3. hatch coaming + lid, offset aft of centre so it clears the dome ring
cyl_along('y', (0.0, 0.50, -0.20), 0.26, 0.22, 8, "hatch", HATCH,
          cap_a=False, phase=math.pi / 8)
# dogs around the coaming
for i in range(4):
    a = math.pi / 4 + i * math.pi / 2
    box((math.cos(a) * 0.27, 0.55, -0.20 + math.sin(a) * 0.27),
        (0.07, 0.05, 0.07), "hatch", BRASS)

# 4. thruster: shrouded 24 V trolling motor, rewound
lathe([(-1.30, 0.30), (-1.56, 0.30)], 8, "shroud",
      lambda p, i, k: shade(STEEL, p, 0.12),
      lambda i, k, p: (i * 0.5, k * 0.5), phase=math.pi / 8)
lathe([(-1.32, 0.26), (-1.54, 0.26)], 8, "shroud",
      lambda p, i, k: shade(DARK, p),
      lambda i, k, p: (i * 0.5, k * 0.5), phase=math.pi / 8)
# stator struts
for i in range(3):
    a = math.pi / 2 + i * 2 * math.pi / 3
    ca, sa = math.cos(a), math.sin(a)
    plate((ca * 0.26, sa * 0.26, -1.34), (0.0, 0.0, -1.34),
          (0.0, 0.0, -1.46), (ca * 0.26, sa * 0.26, -1.46),
          "shroud", STEEL, double=True)
# hub + 4 blades
cyl_along('z', (0, 0, -1.42), 0.08, 0.14, 6, "prop", DARK)
for i in range(4):
    a = i * math.pi / 2 + 0.3
    ca, sa = math.cos(a), math.sin(a)
    pa = (ca * 0.07, sa * 0.07, -1.40)
    pb = (ca * 0.24, sa * 0.24, -1.44)
    pc = (ca * 0.24 - sa * 0.10, sa * 0.24 + ca * 0.10, -1.38)
    pd = (ca * 0.07 - sa * 0.10, sa * 0.07 + ca * 0.10, -1.36)
    plate(pa, pb, pc, pd, "prop", BRASS, double=True)

# 5. landing skids -- scaffold tube, flattened, bolted through pad eyes
for sx in (-1, 1):
    box((sx * 0.34, -0.62, -0.15), (0.10, 0.09, 1.70), "skids", STEEL)
    for z in (-0.75, 0.05, 0.55):
        plate((sx * 0.34, -0.58, z - 0.05), (sx * 0.34, -0.58, z + 0.05),
              (sx * 0.30, -0.30, z + 0.05), (sx * 0.30, -0.30, z - 0.05),
              "skids", STEEL, double=True)

# 6. battery pods / trim tanks -- ex-fire-extinguisher bottles in clamps.
#    They do not match. Two different bottles were what he had.
cyl_along('z', (-0.56, -0.22, -0.16), 0.16, 1.18, 6, "pods", POD)
cyl_along('z', (0.56, -0.20, -0.05), 0.14, 0.96, 6, "pods", (74, 66, 58))
for sx in (-1, 1):
    for z in (-0.55, 0.35):
        box((sx * 0.50, -0.14, z), (0.20, 0.05, 0.07), "pods", STEEL)


# 6b. doubler plates welded over two thin spots, and the cable run
def hull_patch(zc, half_z, half_t, ang, col, r=0.49, group="hull"):
    rf = r * math.cos(math.pi / NSIDE) + 0.014
    nx, ny = math.cos(ang), math.sin(ang)
    tx, ty = -math.sin(ang), math.cos(ang)
    cx, cy = nx * rf, ny * rf
    plate((cx - tx * half_t, cy - ty * half_t, zc - half_z),
          (cx + tx * half_t, cy + ty * half_t, zc - half_z),
          (cx + tx * half_t, cy + ty * half_t, zc + half_z),
          (cx - tx * half_t, cy - ty * half_t, zc + half_z), group, col)


hull_patch(-0.05, 0.24, 0.17, -0.30, (96, 94, 86))
hull_patch(-0.68, 0.15, 0.12, math.pi + 0.62, (104, 92, 74))
for z in (0.44, 0.74):
    box((0.24, 0.44, z), (0.06, 0.06, 0.30), "hull", DARK)
box((0.27, 0.44, 0.10), (0.20, 0.14, 0.26), "hull", (92, 86, 70))

# 7. lamp bar -- two automotive work lights on a length of angle iron
plate((-0.42, 0.16, 0.86), (0.42, 0.16, 0.86),
      (0.42, 0.10, 0.90), (-0.42, 0.10, 0.90), "lights", STEEL, double=True)
for sx in (-1, 1):
    box((sx * 0.36, 0.13, 0.94), (0.16, 0.14, 0.14), "lights", DARK)
    plate((sx * 0.36 - 0.07, 0.20, 1.012), (sx * 0.36 + 0.07, 0.20, 1.012),
          (sx * 0.36 + 0.07, 0.06, 1.012), (sx * 0.36 - 0.07, 0.06, 1.012),
          "lights", LENS)

# 8. specimen basket, nose-under -- rebar frame + fish-farm netting
plate((-0.30, -0.46, 0.62), (0.30, -0.46, 0.62),
      (0.30, -0.46, 1.16), (-0.30, -0.46, 1.16), "basket", GRATE, double=True)
for sx in (-1, 1):
    plate((sx * 0.30, -0.46, 0.62), (sx * 0.30, -0.46, 1.16),
          (sx * 0.30, -0.30, 1.16), (sx * 0.30, -0.30, 0.62),
          "basket", GRATE, double=True)

# 9. stabiliser fins, tail
plate((-0.02, 0.44, -1.28), (0.02, 0.44, -1.28),
      (0.02, 0.72, -1.05), (-0.02, 0.72, -1.05), "fins", STEEL, double=True)
for sx in (-1, 1):
    plate((sx * 0.30, -0.34, -1.26), (sx * 0.30, -0.34, -1.02),
          (sx * 0.60, -0.52, -1.06), (sx * 0.60, -0.52, -1.24),
          "fins", STEEL, double=True)

print(f"verts={len(M.verts)} tris={len(M.tris)}")
from collections import Counter
for g, c in Counter(M.tgrp).most_common():
    print(f"  {g:8s} {c:4d} tris")

# ----------------------------------------------------------------------------
# texture: 32x32 tiling plate + weld + rust
# ----------------------------------------------------------------------------

T = 32
rng = np.random.default_rng(7)
base = np.zeros((T, T, 3), np.float32)
noise = rng.normal(0.0, 1.0, (T, T))
noise = (noise + np.roll(noise, 1, 0) + np.roll(noise, 1, 1)) / 3.0
lum = 0.78 + noise * 0.10
base[..., 0] = lum * 1.00
base[..., 1] = lum * 0.96
base[..., 2] = lum * 0.88
# weld seams (wrap-safe)
base[0:2, :, :] *= 0.72
base[:, 0:2, :] *= 0.78
base[1, :, :] += 0.10
# rivets on the seam
for x in range(2, T, 6):
    base[0:2, x:x + 2, :] = np.clip(base[0:2, x:x + 2, :] + 0.22, 0, 1)
# rust blotches
for _ in range(9):
    cy, cx = rng.integers(0, T, 2)
    r = rng.integers(2, 6)
    for y in range(-r, r + 1):
        for x in range(-r, r + 1):
            d = math.hypot(x, y) / r
            if d > 1:
                continue
            t = (1 - d) * 0.75 * rng.uniform(0.5, 1.0)
            yy, xx = (cy + y) % T, (cx + x) % T
            base[yy, xx, 0] = base[yy, xx, 0] * (1 - t) + 0.62 * t
            base[yy, xx, 1] = base[yy, xx, 1] * (1 - t) + 0.34 * t
            base[yy, xx, 2] = base[yy, xx, 2] * (1 - t) + 0.20 * t
# vertical drip streaks below rust
for _ in range(14):
    x = int(rng.integers(0, T))
    y0 = int(rng.integers(0, T))
    for k in range(int(rng.integers(3, 11))):
        yy = (y0 + k) % T
        f = 0.35 * (1 - k / 11)
        base[yy, x] = base[yy, x] * (1 - f) + np.array([0.55, 0.30, 0.18]) * f
tex = np.clip(base * 255, 0, 255).astype(np.uint8)

from PIL import Image
Image.fromarray(tex).resize((256, 256), Image.NEAREST).save(
    os.path.join(OUT, "loach_hull_x8.png"))
Image.fromarray(tex).save(os.path.join(OUT, "loach_hull.png"))

# RGBA5551 for TMEM
with open(os.path.join(OUT, "loach_hull_rgba16.h"), "w") as f:
    f.write("/* DSV LOACH hull texture -- 32x32 RGBA5551, G_IM_FMT_RGBA/G_IM_SIZ_16b\n"
            "   2048 bytes, half of TMEM. Load with gsDPLoadTextureBlock(...,\n"
            "   G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, G_TX_WRAP, G_TX_WRAP, 5, 5,\n"
            "   G_TX_NOLOD, G_TX_NOLOD). */\n\n")
    f.write("static const unsigned short loach_hull_tex[32*32] = {\n")
    for y in range(T):
        row = []
        for x in range(T):
            r, g, b = (int(v) >> 3 for v in tex[y, x])
            row.append("0x%04X" % ((r << 11) | (g << 6) | (b << 1) | 1))
        f.write("    " + ", ".join(row) + ",\n")
    f.write("};\n")

# ----------------------------------------------------------------------------
# OBJ + MTL
# ----------------------------------------------------------------------------

groups = []
for g in M.tgrp:
    if g not in groups:
        groups.append(g)

with open(os.path.join(OUT, "loach.mtl"), "w") as f:
    f.write("# DSV LOACH\n")
    for g in groups:
        f.write(f"newmtl {g}\nKd 1.0 1.0 1.0\nKa 0 0 0\nKs 0 0 0\nillum 1\n")
        if g in ("hull", "ribs"):
            f.write("map_Kd loach_hull.png\n")
        f.write("\n")

with open(os.path.join(OUT, "loach.obj"), "w") as f:
    f.write("# DSV LOACH -- one-person work submarine\n"
            "# +Z forward, +Y up, 1 unit = 1 m. Vertex colours in v lines.\n")
    f.write("mtllib loach.mtl\n")
    for p, c in zip(M.verts, M.cols):
        f.write("v %.4f %.4f %.4f %.4f %.4f %.4f\n" %
                (p[0], p[1], p[2], c[0] / 255, c[1] / 255, c[2] / 255))
    for u, v in M.uvs:
        f.write("vt %.4f %.4f\n" % (u, v))
    for g in groups:
        f.write(f"g {g}\nusemtl {g}\n")
        for t, tg in zip(M.tris, M.tgrp):
            if tg != g:
                continue
            f.write("f %d/%d %d/%d %d/%d\n" %
                    (t[0] + 1, t[0] + 1, t[1] + 1, t[1] + 1, t[2] + 1, t[2] + 1))

# ----------------------------------------------------------------------------
# F3DEX2 header: 32-entry vertex cache batching, per group
# ----------------------------------------------------------------------------

SCALE = 100.0  # metres -> centimetres, s16
UVSC = 32.0    # texel * 32 (S10.5), texture is 32x32

out_vtx = []       # (x,y,z,s,t,r,g,b,a)
batches = []       # (group, base_index, count, [(i,j,k) local])


def emit_group(g):
    tris = [t for t, tg in zip(M.tris, M.tgrp) if tg == g]
    cur, local, out = {}, [], []
    def flush():
        nonlocal cur, local
        if not local:
            return
        base = len(out_vtx)
        inv = {v: k for k, v in cur.items()}
        for n in range(len(cur)):
            gi = inv[n]
            p = M.verts[gi]
            u, v = M.uvs[gi]
            r, gg, b = M.cols[gi]
            out_vtx.append((int(round(p[0] * SCALE)), int(round(p[1] * SCALE)),
                            int(round(p[2] * SCALE)),
                            int(round(u * UVSC)) & 0xFFFF,
                            int(round(v * UVSC)) & 0xFFFF, r, gg, b, 255))
        batches.append((g, base, len(cur), list(local)))
        cur, local = {}, []
    for t in tris:
        need = sum(1 for i in t if i not in cur)
        if len(cur) + need > 32:
            flush()
        loc = []
        for i in t:
            if i not in cur:
                cur[i] = len(cur)
            loc.append(cur[i])
        local.append(tuple(loc))
    flush()


for g in groups:
    emit_group(g)

with open(os.path.join(OUT, "loach.h"), "w") as f:
    f.write("""/* ---------------------------------------------------------------------------
 * DSV LOACH -- one-person work submarine, N64 asset (F3DEX2)
 * generated by build_loach.py -- do not hand-edit, edit the generator
 *
 * %d verts / %d tris, %d vertex batches (<=32 verts each).
 * Coordinates: s16 centimetres, +Z forward / +Y up. Model is 286 cm long.
 * Shading is BAKED VERTEX COLOUR: run with G_LIGHTING off,
 *   gsSPClearGeometryMode(G_LIGHTING);
 *   gsDPSetCombineMode(G_CC_MODULATERGB, G_CC_MODULATERGB);  (textured groups)
 *   gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE);              (untextured)
 * Groups shroud/prop/skids/fins/basket/lights use single-sided plates and
 * expect gsSPClearGeometryMode(G_CULL_BACK) -- see loach_draw() below.
 * ------------------------------------------------------------------------- */

#ifndef LOACH_H
#define LOACH_H

""" % (len(out_vtx), len(M.tris), len(batches)))
    f.write("static const Vtx loach_vtx[] = {\n")
    for (x, y, z, s, t, r, g, b, a) in out_vtx:
        f.write("    {{{%6d,%6d,%6d}, 0, {%5d,%5d}, {%3d,%3d,%3d,%3d}}},\n"
                % (x, y, z, s, t, r, g, b, a))
    f.write("};\n\n")

    for g in groups:
        f.write(f"static const Gfx loach_dl_{g}[] = {{\n")
        for (bg, base, count, local) in batches:
            if bg != g:
                continue
            f.write(f"    gsSPVertex(&loach_vtx[{base}], {count}, 0),\n")
            i = 0
            while i + 1 < len(local):
                a1, b1, c1 = local[i]
                a2, b2, c2 = local[i + 1]
                f.write("    gsSP2Triangles(%2d,%2d,%2d, 0, %2d,%2d,%2d, 0),\n"
                        % (a1, b1, c1, a2, b2, c2))
                i += 2
            if i < len(local):
                a1, b1, c1 = local[i]
                f.write("    gsSP1Triangle(%2d,%2d,%2d, 0),\n" % (a1, b1, c1))
        f.write("    gsSPEndDisplayList(),\n};\n\n")

    solid = [g for g in groups if g in ("hull", "ribs", "dome", "hatch", "pods")]
    thin = [g for g in groups if g not in solid]
    f.write("static const Gfx loach_dl[] = {\n"
            "    gsSPClearGeometryMode(G_LIGHTING),\n"
            "    gsSPSetGeometryMode(G_CULL_BACK | G_SHADING_SMOOTH),\n")
    for g in solid:
        f.write(f"    gsSPDisplayList(loach_dl_{g}),\n")
    f.write("    gsSPClearGeometryMode(G_CULL_BACK),  /* thin plates below */\n")
    for g in thin:
        f.write(f"    gsSPDisplayList(loach_dl_{g}),\n")
    f.write("    gsSPSetGeometryMode(G_CULL_BACK),\n"
            "    gsSPEndDisplayList(),\n};\n\n#endif /* LOACH_H */\n")

# ----------------------------------------------------------------------------
# preview
# ----------------------------------------------------------------------------

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

V = np.array(M.verts)[:, [0, 2, 1]]   # model is Y-up/+Z fwd; mpl wants Z-up
C = np.array(M.cols) / 255.0
F = np.array(M.tris)
polys = V[F]
n = np.cross(polys[:, 1] - polys[:, 0], polys[:, 2] - polys[:, 0])
ln = np.linalg.norm(n, axis=1, keepdims=True)
ln[ln == 0] = 1
n = n / ln
L = np.array([-0.45, 0.55, 0.70])
L = L / np.linalg.norm(L)
lam = 0.55 + 0.75 * np.clip(n @ L, 0, 1)
fc = np.clip(C[F].mean(axis=1) * 1.5 * lam[:, None], 0, 1)

views = [(16, 132, "three-quarter, bow-port"), (2, 180, "port elevation"),
         (4, 92, "bow"), (88, 180, "plan")]
fig = plt.figure(figsize=(12, 9), facecolor="#0b0e10")
for k, (el, az, name) in enumerate(views):
    ax = fig.add_subplot(2, 2, k + 1, projection="3d", facecolor="#0b0e10")
    pc = Poly3DCollection(polys, facecolors=fc, edgecolors=(0, 0, 0, 0.35),
                          linewidths=0.3)
    ax.add_collection3d(pc)
    ax.set_xlim(-1.0, 1.0); ax.set_ylim(-1.7, 1.7); ax.set_zlim(-1.0, 1.0)
    ax.set_box_aspect((2.0, 3.4, 2.0), zoom=1.45)
    ax.view_init(elev=el, azim=az)
    ax.set_axis_off()
    ax.set_title(name, color="#9fb0a6", fontsize=10, family="monospace")
fig.suptitle("DSV LOACH  --  %d tris / %d verts" % (len(M.tris), len(out_vtx)),
             color="#c9d6cc", family="monospace", fontsize=14)
fig.subplots_adjust(left=0.0, right=1.0, top=0.94, bottom=0.0,
                    wspace=0.0, hspace=0.05)
fig.savefig(os.path.join(OUT, "loach_preview.png"), dpi=115,
            facecolor="#0b0e10")
print("wrote", OUT)
