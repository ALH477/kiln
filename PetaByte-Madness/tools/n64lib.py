import numpy as np
from PIL import Image

# ---------------------------------------------------------------- mesh
class Mesh:
    def __init__(self):
        self.V = []          # list of (x,y,z)
        self.F = []          # list of (i0,i1,i2)
        self.C = []          # per-face rgb (fallback when untextured)
        self.G = []          # per-face group name
        self.FUV = []        # per-face [(u,v)]*3, or None
        self.T = []          # per-face texture index, or -1
        self.VG = []         # per-vertex group name (for rigging)

    def add(self, verts, faces, color, group="body", uv=None, tex=-1):
        """color may be one rgb tuple, or one per face (keeps verts shared -> smooth)."""
        b = len(self.V)
        P = np.asarray(verts, dtype=np.float64)
        self.V.extend([tuple(v) for v in verts])
        self.VG.extend([group] * len(verts))
        pc = P.mean(axis=0)
        tex_base = len(self.T)
        cols = color if (isinstance(color, list) and len(color) == len(faces)) \
            else [color] * len(faces)
        for f, color in zip(faces, cols):
            if uv is not None:
                t = [list(uv[f[0]]), list(uv[f[1]]), list(uv[f[2]])]
                us = [c[0] for c in t]                      # unwrap the u seam
                if max(us) - min(us) > 0.5:
                    for c in t:
                        if c[0] < 0.5:
                            c[0] += 1.0
                self.FUV.append([tuple(c) for c in t])
            else:
                self.FUV.append(None)
            self.T.append(tex if not isinstance(tex, list) else tex[len(self.T) - tex_base])
            a, bb, c = P[f[0]], P[f[1]], P[f[2]]
            n = np.cross(bb - a, c - a)
            if n @ ((a + bb + c) / 3.0 - pc) < 0:      # flip inward-wound faces
                f = (f[0], f[2], f[1])
            self.F.append((b + f[0], b + f[1], b + f[2]))
            self.C.append(color)
            self.G.append(group)
        return self

    def merge(self, other, offset=(0, 0, 0), rot_y=0.0, scale=1.0, regroup=None):
        c, s = np.cos(rot_y), np.sin(rot_y)
        b = len(self.V)
        for (x, y, z), vg in zip(other.V, other.VG):
            x, y, z = x * scale, y * scale, z * scale
            self.V.append((x * c + z * s + offset[0], y + offset[1], -x * s + z * c + offset[2]))
            self.VG.append(regroup or vg)
        for k, (f, col, g) in enumerate(zip(other.F, other.C, other.G)):
            g = regroup or g
            self.F.append((b + f[0], b + f[1], b + f[2]))
            self.C.append(col)
            self.G.append(g)
            self.FUV.append(other.FUV[k] if k < len(other.FUV) else None)
            self.T.append(other.T[k] if k < len(other.T) else -1)
        return self

    def copy(self):
        m = Mesh()
        m.V, m.F, m.C, m.G, m.VG = list(self.V), list(self.F), list(self.C), \
            list(self.G), list(self.VG)
        m.FUV, m.T = list(self.FUV), list(self.T)
        return m

    def arrays(self):
        return np.array(self.V, dtype=np.float64), np.array(self.F, dtype=np.int32), \
               np.array(self.C, dtype=np.float64) / 255.0

    def tri_count(self):
        return len(self.F)

    def vert_count(self):
        return len(self.V)

    def write_obj(self, path, name="model", mtllib=None, texnames=None):
        V, F, _ = self.arrays()
        groups = {}
        for i, g in enumerate(self.G):
            groups.setdefault(g, []).append(i)
        vt, vtmap = [], {}
        for corners in self.FUV:
            if corners:
                for c in corners:
                    if c not in vtmap:
                        vtmap[c] = len(vt) + 1
                        vt.append(c)
        with open(path, "w") as fh:
            fh.write(f"# {name} -- N64-budget low-poly\n")
            fh.write(f"# {len(V)} verts / {len(F)} tris\n")
            if mtllib:
                fh.write(f"mtllib {mtllib}\n")
            for v in V:
                fh.write("v %.4f %.4f %.4f\n" % tuple(v))
            for u, w in vt:
                fh.write("vt %.5f %.5f\n" % (u, 1.0 - w))
            for g, idxs in groups.items():
                fh.write(f"g {g}\n")
                cur = None
                for i in idxs:
                    t = self.T[i]
                    if texnames and t >= 0 and t != cur:
                        fh.write(f"usemtl {texnames[t]}\n"); cur = t
                    a, b, c = self.F[i]
                    cn = self.FUV[i]
                    if cn:
                        ta, tb, tc = (vtmap[cn[0]], vtmap[cn[1]], vtmap[cn[2]])
                        fh.write(f"f {a+1}/{ta} {b+1}/{tb} {c+1}/{tc}\n")
                    else:
                        fh.write(f"f {a+1} {b+1} {c+1}\n")


def _n(v):
    v = np.asarray(v, dtype=np.float64)
    return v / (np.linalg.norm(v) + 1e-12)


def box(cx, cy, cz, sx, sy, sz, taper=(1.0, 1.0), lean=0.0, tilt=0.0, twist=0.0):
    """Axis-aligned box, optionally tapered at the top, leaned (z) or tilted (x)."""
    hx, hz = sx / 2, sz / 2
    tx, tz = hx * taper[0], hz * taper[1]
    bot = [(-hx, 0, -hz), (hx, 0, -hz), (hx, 0, hz), (-hx, 0, hz)]
    top = [(-tx, sy, -tz), (tx, sy, -tz), (tx, sy, tz), (-tx, sy, tz)]
    V = []
    for (x, y, z) in bot + top:
        if twist:
            c, s = np.cos(twist), np.sin(twist)
            x, z = x * c + z * s, -x * s + z * c
        frac = y / sy if sy else 0
        x += lean * frac * sy
        z += tilt * frac * sy
        V.append((cx + x, cy + y, cz + z))
    F = [(0, 2, 1), (0, 3, 2),        # bottom
         (4, 5, 6), (4, 6, 7),        # top
         (0, 1, 5), (0, 5, 4),        # -z
         (1, 2, 6), (1, 6, 5),        # +x
         (2, 3, 7), (2, 7, 6),        # +z
         (3, 0, 4), (3, 4, 7)]        # -x
    return V, F


def seg_box(p0, p1, w, h, w2=None, h2=None):
    """Oriented box running from p0 to p1 -- for magazines, grips, stocks."""
    p0, p1 = np.asarray(p0, float), np.asarray(p1, float)
    ax = _n(p1 - p0)
    up = np.array([0.0, 1.0, 0.0])
    if abs(ax @ up) > 0.97:
        up = np.array([0.0, 0.0, 1.0])
    r = _n(np.cross(ax, up)); u = np.cross(r, ax)
    w2 = w if w2 is None else w2
    h2 = h if h2 is None else h2
    V = [p0 - r * w / 2 - u * h / 2, p0 + r * w / 2 - u * h / 2,
         p0 + r * w / 2 + u * h / 2, p0 - r * w / 2 + u * h / 2,
         p1 - r * w2 / 2 - u * h2 / 2, p1 + r * w2 / 2 - u * h2 / 2,
         p1 + r * w2 / 2 + u * h2 / 2, p1 - r * w2 / 2 + u * h2 / 2]
    F = [(0, 1, 2), (0, 2, 3), (4, 6, 5), (4, 7, 6), (0, 4, 5), (0, 5, 1),
         (1, 5, 6), (1, 6, 2), (2, 6, 7), (2, 7, 3), (3, 7, 4), (3, 4, 0)]
    return [tuple(v) for v in V], F


def wedge(cx, cy, cz, sx, sy, sz):
    """Triangular prism pointing +z (nose, sight posts, muzzle cones)."""
    hx = sx / 2
    V = [(cx - hx, cy, cz - sz / 2), (cx + hx, cy, cz - sz / 2),
         (cx + hx, cy + sy, cz - sz / 2), (cx - hx, cy + sy, cz - sz / 2),
         (cx, cy + sy / 2, cz + sz / 2)]
    F = [(0, 2, 1), (0, 3, 2), (0, 1, 4), (1, 2, 4), (2, 3, 4), (3, 0, 4)]
    return V, F


def cyl(cx, cy, cz, r, h, seg=6, axis="y", r2=None):
    """Low-segment cylinder / cone."""
    r2 = r if r2 is None else r2
    V, F = [], []
    for i in range(seg):
        a = 2 * np.pi * i / seg
        ca, sa = np.cos(a), np.sin(a)
        if axis == "y":
            V.append((cx + r * ca, cy, cz + r * sa))
        elif axis == "z":
            V.append((cx + r * ca, cy + r * sa, cz))
        else:
            V.append((cx, cy + r * ca, cz + r * sa))
    for i in range(seg):
        a = 2 * np.pi * i / seg
        ca, sa = np.cos(a), np.sin(a)
        if axis == "y":
            V.append((cx + r2 * ca, cy + h, cz + r2 * sa))
        elif axis == "z":
            V.append((cx + r2 * ca, cy + r2 * sa, cz + h))
        else:
            V.append((cx + h, cy + r2 * ca, cz + r2 * sa))
    for i in range(seg):
        j = (i + 1) % seg
        F += [(i, j, seg + j), (i, seg + j, seg + i)]
    for i in range(1, seg - 1):
        F += [(0, i + 1, i), (seg, seg + i, seg + i + 1)]
    return V, F


def blob(cx, cy, cz, rx, ry, rz, seg=8, rings=4, warp=None):
    """Low-poly spheroid. `warp(x,y,z)->(x,y,z)` deforms it in unit space first."""
    V, F = [], []
    for j in range(1, rings + 1):
        phi = np.pi * j / (rings + 1)
        sy_, sr = np.cos(phi), np.sin(phi)
        for i in range(seg):
            th = 2 * np.pi * i / seg
            V.append((sr * np.cos(th), sy_, sr * np.sin(th)))
    V.append((0.0, 1.0, 0.0)); V.append((0.0, -1.0, 0.0))
    top, bot = len(V) - 2, len(V) - 1
    for j in range(rings - 1):
        for i in range(seg):
            a = j * seg + i; b = j * seg + (i + 1) % seg
            c = (j + 1) * seg + i; d = (j + 1) * seg + (i + 1) % seg
            F += [(a, b, d), (a, d, c)]
    for i in range(seg):
        F.append((top, i, (i + 1) % seg))
        F.append((bot, (rings - 1) * seg + (i + 1) % seg, (rings - 1) * seg + i))
    out = []
    for (x, y, z) in V:
        if warp is not None:
            x, y, z = warp(x, y, z)
        out.append((cx + x * rx, cy + y * ry, cz + z * rz))
    return out, F


def blob_uv(cx, cy, cz, rx, ry, rz, seg=8, rings=4, warp=None, uspan=1.0, uoff=0.5):
    """blob() plus spherical UVs: u=uoff faces +Z, v=0 at the crown."""
    V, F = blob(cx, cy, cz, rx, ry, rz, seg, rings, warp)
    UV = []
    for j in range(1, rings + 1):
        phi = np.pi * j / (rings + 1)
        for i in range(seg):
            th = 2 * np.pi * i / seg
            x, z = np.cos(th), np.sin(th)
            UV.append(((uoff + uspan * np.arctan2(x, z) / (2 * np.pi)) % 1.0,
                       phi / np.pi))
    UV.append((uoff, 0.002)); UV.append((uoff, 0.998))
    return V, F, UV


# ---------------------------------------------------------------- render
def render(mesh, W=520, H=520, cam=(1.6, 0.9, 2.4), target=(0, 0.5, 0),
           fov=32.0, bg=(30, 33, 39), ss=2, ambient=0.55, outline=True, key_dir=None, smooth=True, double_sided=False, textures=None):
    V, F, C = mesh.arrays()
    if len(F) == 0:
        return Image.new("RGB", (W, H), bg)
    Wp, Hp = W * ss, H * ss
    cam = np.array(cam, dtype=np.float64)
    tgt = np.array(target, dtype=np.float64)

    fwd = _n(tgt - cam)
    right = _n(np.cross(fwd, np.array([0.0, 1.0, 0.0])))
    up = np.cross(right, fwd)
    M = np.stack([right, up, -fwd])
    cs = (V - cam) @ M.T
    z = -cs[:, 2]
    z = np.maximum(z, 1e-4)
    tanf = np.tan(np.radians(fov) / 2)
    aspect = Wp / Hp
    sx = (cs[:, 0] / (z * tanf * aspect) + 1) * 0.5 * Wp
    sy = (1 - cs[:, 1] / (z * tanf)) * 0.5 * Hp
    P = np.stack([sx, sy, z], axis=1)

    # world-space flat normals
    a, b, c = V[F[:, 0]], V[F[:, 1]], V[F[:, 2]]
    nrm = np.cross(b - a, c - a)
    nrm /= (np.linalg.norm(nrm, axis=1, keepdims=True) + 1e-12)
    cen = (a + b + c) / 3.0
    if double_sided:
        nrm[np.einsum('ij,ij->i', nrm, cam - cen) < 0] *= -1.0
    key = _n(key_dir if key_dir is not None else [0.55, 0.8, 0.62])
    fill = _n(np.cross(key, [0.0, 1.0, 0.0]) + np.array([0.0, 0.25, 0.0]))
    def _lit(n):
        return ambient + 0.66 * np.clip(n @ key, 0, 1) + 0.26 * np.clip(n @ fill, 0, 1)
    lam = _lit(nrm)
    shade = np.clip(C * lam[:, None], 0, 1)
    if smooth:
        VN = np.zeros_like(V)
        for k in range(3):
            np.add.at(VN, F[:, k], nrm)
        VN /= (np.linalg.norm(VN, axis=1, keepdims=True) + 1e-12)

    buf = np.zeros((Hp, Wp, 3), dtype=np.float32)
    buf[:] = np.array(bg, dtype=np.float32) / 255.0
    zbuf = np.full((Hp, Wp), 1e9, dtype=np.float32)

    order = np.argsort(-P[F, 2].mean(axis=1))
    for fi in order:
        i0, i1, i2 = F[fi]
        p0, p1, p2 = P[i0], P[i1], P[i2]
        area = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1])
        if abs(area) < 1e-9:
            continue
        x0 = max(int(np.floor(min(p0[0], p1[0], p2[0]))), 0)
        x1 = min(int(np.ceil(max(p0[0], p1[0], p2[0]))) + 1, Wp)
        y0 = max(int(np.floor(min(p0[1], p1[1], p2[1]))), 0)
        y1 = min(int(np.ceil(max(p0[1], p1[1], p2[1]))) + 1, Hp)
        if x1 <= x0 or y1 <= y0:
            continue
        xs = np.arange(x0, x1) + 0.5
        ys = np.arange(y0, y1) + 0.5
        gx, gy = np.meshgrid(xs, ys)
        w0 = ((p1[0] - p0[0]) * (gy - p0[1]) - (gx - p0[0]) * (p1[1] - p0[1])) / area
        w1 = ((gx - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (gy - p0[1])) / area
        inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w0 + w1 <= 1 + 1e-6)
        if not inside.any():
            continue
        zi = p0[2] + w1 * (p1[2] - p0[2]) + w0 * (p2[2] - p0[2])
        sub = zbuf[y0:y1, x0:x1]
        m = inside & (zi < sub)
        if not m.any():
            continue
        sub[m] = zi[m]
        tile = buf[y0:y1, x0:x1]
        col = shade[fi]
        base = None
        if textures is not None and mesh.T[fi] >= 0 and mesh.FUV[fi]:
            tex = textures[mesh.T[fi]]
            th_, tw_ = tex.shape[:2]
            (ua, va), (ub, vb), (uc, vc) = mesh.FUV[fi]
            w2t = 1.0 - w0 - w1
            uu = ua * w2t + ub * w1 + uc * w0
            vv = va * w2t + vb * w1 + vc * w0
            px = np.clip((uu % 1.0) * tw_, 0, tw_ - 1).astype(np.int32)
            py = np.clip(vv * th_, 0, th_ - 1).astype(np.int32)
            base = tex[py, px]
        if smooth:
            w2 = 1.0 - w0 - w1
            pn = (VN[i0] * w2[..., None] + VN[i1] * w1[..., None] + VN[i2] * w0[..., None])
            pn /= (np.linalg.norm(pn, axis=-1, keepdims=True) + 1e-12)
            src = C[fi] if base is None else base
            col = np.clip(src * _lit(pn)[..., None], 0, 1)
            tile[m] = col[m]
            if outline:
                edge = (np.minimum(np.minimum(w0, w1), w2) < 0.018)
                tile[m & edge] = col[m & edge] * 0.68
            continue
        if outline:
            edge = (np.minimum(np.minimum(w0, w1), 1 - w0 - w1) < 0.030)
            tile[m & ~edge] = col
            tile[m & edge] = col * 0.52
        else:
            tile[m] = col

    img = Image.fromarray((np.clip(buf, 0, 1) * 255).astype(np.uint8))
    return img.resize((W, H), Image.LANCZOS)


def turntable(mesh, frames=24, radius=2.5, height=0.95, target=(0, 0.5, 0), **kw):
    out = []
    for i in range(frames):
        a = 2 * np.pi * i / frames
        cam = (np.sin(a) * radius, height, np.cos(a) * radius)
        kd = [np.sin(a + 0.55) * 0.72, 0.72, np.cos(a + 0.55) * 0.72]
        out.append(render(mesh, cam=cam, target=target, key_dir=kd, **kw))
    return out


def render_fit(mesh, W=520, H=300, az=-1.35, el=0.30, fov=26.0, margin=1.22, **kw):
    """Auto-frame a mesh: az/el in radians around its own bounding-box centre."""
    V = np.array(mesh.V, dtype=np.float64)
    lo, hi = V.min(axis=0), V.max(axis=0)
    c = (lo + hi) / 2
    r = np.linalg.norm(hi - lo) / 2
    d = margin * r / np.tan(np.radians(fov) / 2)
    cam = (c[0] + d * np.sin(az) * np.cos(el), c[1] + d * np.sin(el),
           c[2] + d * np.cos(az) * np.cos(el))
    return render(mesh, W=W, H=H, cam=cam, target=tuple(c), fov=fov, **kw)
