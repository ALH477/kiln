#!/usr/bin/env python3
"""
mc_preview.py — z-buffered software render of machine_centaur.obj so the
silhouette can be checked without leaving the shell.  Reads vertex colours
straight out of the extended OBJ, applies a single key light plus the baked
per-vertex AO, and writes front / side / three-quarter PNGs.
"""
import math
import sys
import os

import numpy as np
from PIL import Image

W, H = 420, 460
BG = (16, 17, 20)


TEXTURED = {"face": "mc_face.png", "hull": "mc_plate.png",
            "gore": "mc_gore.png"}


def load_obj(path):
    """Returns verts, vertex colours, and faces as (a,b,c,uvs,texname|None).

    Textured groups get their colour from a nearest-neighbour texel fetch at
    the face centroid — crude, but enough to catch a mirrored or inverted UV,
    which is the one texture bug that is invisible in a flat-shaded preview.
    """
    V, C, T, F = [], [], [], []
    tex, group = {}, None
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "v":
            V.append([float(p[1]), float(p[2]), float(p[3])])
            C.append([float(p[4]), float(p[5]), float(p[6])]
                     if len(p) >= 7 else [0.8, 0.8, 0.8])
        elif p[0] == "vt":
            T.append([float(p[1]), float(p[2])])
        elif p[0] == "g":
            # "<bone>__<material>" since the rig went in — texture binding
            # keys off the material half only
            group = p[1].split("__")[-1]
            if group in TEXTURED and group not in tex:
                # resolve next to the .obj, not the cwd — the generator writes
                # into out/ and every caller runs from the repo root, so a bare
                # relative name silently loses every texture and the model
                # renders as untextured grey
                cand = os.path.join(os.path.dirname(os.path.abspath(path)),
                                    TEXTURED[group])
                try:
                    tex[group] = np.asarray(
                        Image.open(cand).convert("RGB"),
                        dtype=np.float64) / 255.0
                except OSError:
                    tex[group] = None
        elif p[0] == "f":
            raw = [t.split("/") for t in p[1:]]
            idx = [int(r[0]) - 1 for r in raw]
            uvi = [int(r[1]) - 1 if len(r) > 1 and r[1] else -1 for r in raw]
            for k in range(1, len(idx) - 1):
                F.append((idx[0], idx[k], idx[k + 1],
                          (uvi[0], uvi[k], uvi[k + 1]), group))
    return np.array(V), np.array(C), np.array(T) if T else None, F, tex


def texel(tex, T, uvi):
    """Nearest texel at the centroid of the triangle's UVs."""
    if tex is None or T is None or any(i < 0 for i in uvi):
        return None
    u = float(np.mean([T[i][0] for i in uvi]))
    v = float(np.mean([T[i][1] for i in uvi]))
    h, w, _ = tex.shape
    x = int(min(w - 1, max(0, round(u * (w - 1)))))
    y = int(min(h - 1, max(0, round((1.0 - v) * (h - 1)))))
    return tex[y, x]


def look_at(eye, tgt, up=(0, 1, 0)):
    f = np.array(tgt) - np.array(eye); f = f / np.linalg.norm(f)
    r = np.cross(f, up); r = r / np.linalg.norm(r)
    u = np.cross(r, f)
    Mv = np.eye(4)
    Mv[0, :3], Mv[1, :3], Mv[2, :3] = r, u, -f
    Mv[:3, 3] = -Mv[:3, :3] @ np.array(eye)
    return Mv


def render(V, C, T, F, tex, eye, tgt, fov=42.0, path="out.png"):
    Mv = look_at(eye, tgt)
    hom = np.hstack([V, np.ones((len(V), 1))])
    cam = (Mv @ hom.T).T[:, :3]

    fpx = (H * 0.5) / math.tan(math.radians(fov) * 0.5)
    img = np.full((H, W, 3), BG, dtype=np.float64)
    zbuf = np.full((H, W), 1e18)

    light = np.array([0.45, 0.72, 0.53]); light /= np.linalg.norm(light)

    def project(p):
        if p[2] > -1.0:
            return None
        return (W * 0.5 + fpx * p[0] / -p[2], H * 0.5 - fpx * p[1] / -p[2], -p[2])

    tris = []
    for (a, b, c, uvi, grp) in F:
        pa, pb, pc = cam[a], cam[b], cam[c]
        n = np.cross(pb - pa, pc - pa)
        nl = np.linalg.norm(n)
        if nl < 1e-9:
            continue
        n = n / nl
        if np.dot(n, pa) > 0:          # backface in camera space
            n = -n
            shade_two_sided = True
        else:
            shade_two_sided = False
        lam = 0.34 + 0.66 * max(0.0, float(np.dot(n, light)))
        base = (C[a] + C[b] + C[c]) / 3.0
        s = [project(pa), project(pb), project(pc)]
        if any(x is None for x in s):
            continue
        tmap = tex.get(grp) if grp in tex else None
        uvs = None
        if tmap is not None and T is not None and all(i >= 0 for i in uvi):
            uvs = [T[i] for i in uvi]
        tris.append((s, np.clip(base, 0, 1) * lam * 255.0, uvs, tmap,
                     base * lam))

    for (s, col, uvs, tmap, shaded) in tris:
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = s
        minx = max(0, int(min(x0, x1, x2)));  maxx = min(W - 1, int(max(x0, x1, x2)) + 1)
        miny = max(0, int(min(y0, y1, y2)));  maxy = min(H - 1, int(max(y0, y1, y2)) + 1)
        if minx > maxx or miny > maxy:
            continue
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9:
            continue
        ys, xs = np.mgrid[miny:maxy + 1, minx:maxx + 1]
        px, py = xs + 0.5, ys + 0.5
        w0 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / area
        w1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / area
        w2 = 1.0 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        z = w2 * z0 + w1 * z1 + w0 * z2
        sub = zbuf[miny:maxy + 1, minx:maxx + 1]
        m = inside & (z < sub)
        sub[m] = z[m]
        dst = img[miny:maxy + 1, minx:maxx + 1]
        if uvs is None:
            dst[m] = np.clip(col, 0, 255)
        else:
            # barycentric weights map to verts 0,1,2 as w2,w1,w0
            u = w2 * uvs[0][0] + w1 * uvs[1][0] + w0 * uvs[2][0]
            v = w2 * uvs[0][1] + w1 * uvs[1][1] + w0 * uvs[2][1]
            th, tw, _ = tmap.shape
            tx = np.clip((u * (tw - 1)).round().astype(int), 0, tw - 1)
            ty = np.clip(((1.0 - v) * (th - 1)).round().astype(int), 0, th - 1)
            texel_rgb = tmap[ty, tx]
            out = np.clip(texel_rgb * shaded[None, None, :] * 1.9, 0, 1) * 255.0
            dst[m] = out[m]

    Image.fromarray(img.astype(np.uint8)).save(path)


if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else "out/machine_centaur.obj"
    V, C, T, F, tex = load_obj(src)
    tgt = (0.0, 135.0, 0.0)
    render(V, C, T, F, tex, (0, 160, 520), tgt, path="out/view_front.png")
    render(V, C, T, F, tex, (520, 160, 30), tgt, path="out/view_side.png")
    render(V, C, T, F, tex, (370, 235, 380), tgt, path="out/view_3q.png")
    render(V, C, T, F, tex, (95, 222, 175), (0, 214, 10), fov=40,
           path="out/view_head.png")
    print("ok — %d verts / %d tris" % (len(V), len(F)))
