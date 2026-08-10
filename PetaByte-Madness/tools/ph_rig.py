#!/usr/bin/env python3
"""
ph_rig.py — skeleton, rigid vertex binding and FK for the Horner model.

Seventeen joints, one joint per vertex.  Rigid binding rather than smooth
skinning is the deliberate choice here: the RSP has no skinning path, so
every N64 character was a tree of rigid segments each drawn under its own
gSPMatrix.  Blending on the host would produce a preview the console can
never match.  The elbow and knee bulges in the mesh exist to fill the wedge
that rigid segments open when they bend.

The bind pose is the slumped standing mesh, so an all-zero pose IS the
character as delivered.  Every animation carries corrective angles away from
that, which means the hunch survives into poses where a straight-spine rig
would have quietly lost it.

Emits ph_rig.h: joint table plus one Vtx array and display list per joint.

Usage:  python3 ph_rig.py [--scale 8] [--outdir .]
"""

import argparse
import math
import os

import numpy as np

import patrick_horner_gen as G

# name, parent, pivot in UPRIGHT space, group whose posture transform applies.
# Anatomical sides: he faces +Z with +Y up, so his right hand is at -X.
JOINTS = [
    ("root",   None,     (  0.0,  96.0, 0.0), "torso"),
    ("spine",  "root",   (  0.0, 116.0, 0.0), "torso"),
    ("chest",  "spine",  (  0.0, 142.0, 0.0), "torso"),
    ("neck",   "chest",  (  0.0, 157.0, 0.0), "neck"),
    ("head",   "neck",   (  0.0, 166.0, 1.0), "head"),
    ("clav_R", "chest",  (-21.6, 153.0, 0.0), "arm"),
    ("elb_R",  "clav_R", (-23.4, 118.0, 0.0), "arm"),
    ("wri_R",  "elb_R",  (-24.8,  88.0, 0.0), "arm"),
    ("clav_L", "chest",  ( 21.6, 153.0, 0.0), "arm"),
    ("elb_L",  "clav_L", ( 23.4, 118.0, 0.0), "arm"),
    ("wri_L",  "elb_L",  ( 24.8,  88.0, 0.0), "arm"),
    ("hip_R",  "root",   (-10.4,  92.0, 0.0), "leg"),
    ("kne_R",  "hip_R",  (-10.0,  52.0, 0.0), "leg"),
    ("ank_R",  "kne_R",  ( -9.8,  10.0, 0.0), "leg"),
    ("hip_L",  "root",   ( 10.4,  92.0, 0.0), "leg"),
    ("kne_L",  "hip_L",  ( 10.0,  52.0, 0.0), "leg"),
    ("ank_L",  "kne_L",  (  9.8,  10.0, 0.0), "leg"),
]

NAMES = [j[0] for j in JOINTS]
INDEX = {n: i for i, n in enumerate(NAMES)}
NJ = len(JOINTS)


def bind_joint(group, x, y):
    """Which joint owns a vertex, from its UPRIGHT position and group.

    Thresholds sit between the mesh's cross-section rings, never on one, so
    a whole ring always lands in the same segment."""
    side = "L" if x > 0.0 else "R"
    if group == "head":
        return INDEX["head"]
    if group == "neck":
        return INDEX["neck"]
    if group == "arm":
        if y >= 136.0:
            return INDEX["clav_" + side]
        if y >= 100.0:
            return INDEX["elb_" + side]
        return INDEX["wri_" + side]
    if group == "hand":
        return INDEX["wri_" + side]
    if group == "leg":
        if y >= 72.0:
            return INDEX["hip_" + side]
        if y >= 31.0:
            return INDEX["kne_" + side]
        return INDEX["ank_" + side]
    if group == "foot":
        return INDEX["ank_" + side]
    if y >= 148.0:                      # torso, coat, anything else
        return INDEX["chest"]
    if y >= 106.0:
        return INDEX["spine"]
    return INDEX["root"]


def rot_x(d):
    c, s = math.cos(math.radians(d)), math.sin(math.radians(d))
    return np.array([[1, 0, 0, 0], [0, c, -s, 0], [0, s, c, 0], [0, 0, 0, 1]],
                    float)


def rot_y(d):
    c, s = math.cos(math.radians(d)), math.sin(math.radians(d))
    return np.array([[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0], [0, 0, 0, 1]],
                    float)


def rot_z(d):
    c, s = math.cos(math.radians(d)), math.sin(math.radians(d))
    return np.array([[c, -s, 0, 0], [s, c, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]],
                    float)


def euler(rx, ry, rz):
    """Z then Y then X, matching the order the exported s16 triple is read."""
    return rot_x(rx) @ rot_y(ry) @ rot_z(rz)


def translate(v):
    m = np.eye(4)
    m[:3, 3] = v
    return m


class Rig:
    """Bound character.  build() leaves G.POS holding the bind pose."""

    def __init__(self):
        G.POS.clear(); G.COL.clear(); G.UVS.clear()
        G.GRP.clear(); G.TRIS.clear()
        G.build(upright=True)
        self.bind_of = np.array(
            [bind_joint(G.GRP[i], G.POS[i][0], G.POS[i][1])
             for i in range(len(G.POS))], int)
        self.pivot = np.array(
            [G.slump_point(p[0], p[1], p[2], grp) for _, _, p, grp in JOINTS],
            float)
        G.slump()
        self.rest = np.array(G.POS, float)
        self.col = list(G.COL)
        self.uv = list(G.UVS)
        self.tris = [(a, b, c, m) for a, b, c, m, _ in G.TRIS]
        # A triangle is drawn under ONE matrix on hardware, so it belongs to
        # the joint its first vertex binds to.  Previewing with per-vertex
        # blending instead would show smooth joints the RSP cannot produce.
        self.tri_joint = np.array([int(self.bind_of[t[0]]) for t in self.tris],
                                  int)
        self.parent = [INDEX[p] if p else -1 for _, p, _, _ in JOINTS]
        # local rest offset of each joint from its parent
        self.local = np.array(
            [self.pivot[i] - (self.pivot[self.parent[i]]
                              if self.parent[i] >= 0 else np.zeros(3))
             for i in range(NJ)], float)

    def matrices(self, pose):
        """pose: (NJ, 3) euler degrees.  Returns per-joint world matrices."""
        out = [None] * NJ
        for i in range(NJ):
            local = translate(self.local[i]) @ euler(*pose[i])
            p = self.parent[i]
            out[i] = local if p < 0 else out[p] @ local
        return out

    def pose_segments(self, pose, root_pos=None, root_yaw=0.0,
                      root_pitch=0.0):
        """Expanded buffer: three verts per triangle, each transformed by the
        triangle's own segment matrix.  This is what the console draws,
        joint seams included."""
        mats = self.matrices(pose)
        idx = np.array([t[:3] for t in self.tris], int).reshape(-1)
        jj = np.repeat(self.tri_joint, 3)
        out = np.empty((len(idx), 3), float)
        for j in range(NJ):
            sel = jj == j
            if not sel.any():
                continue
            local = self.rest[idx[sel]] - self.pivot[j]
            out[sel] = local @ mats[j][:3, :3].T + mats[j][:3, 3]
        if root_pos is not None:
            R = (rot_y(root_yaw) @ rot_x(root_pitch))[:3, :3]
            out = (out - self.pivot[0]) @ R.T + np.asarray(root_pos, float)
        return out, idx

    def pose_vertices(self, pose, root_pos=None, root_yaw=0.0, root_pitch=0.0):
        mats = self.matrices(pose)
        v = np.empty_like(self.rest)
        for j in range(NJ):
            sel = self.bind_of == j
            if not sel.any():
                continue
            local = self.rest[sel] - self.pivot[j]
            v[sel] = local @ mats[j][:3, :3].T + mats[j][:3, 3]
        if root_pos is not None:
            R = (rot_y(root_yaw) @ rot_x(root_pitch))[:3, :3]
            v = (v - self.pivot[0]) @ R.T + np.asarray(root_pos, float)
        return v


# --------------------------------------------------------------------------
# exporter
# --------------------------------------------------------------------------
def batch(tris, cache=32):
    out, verts, remap, local = [], [], {}, []
    for a, b, c in tris:
        need = [v for v in dict.fromkeys((a, b, c)) if v not in remap]
        if len(verts) + len(need) > cache:
            out.append((verts, local))
            verts, remap, local = [], {}, []
            need = list(dict.fromkeys((a, b, c)))
        for v in need:
            remap[v] = len(verts)
            verts.append(v)
        local.append((remap[a], remap[b], remap[c]))
    if local:
        out.append((verts, local))
    return out


def write_header(rig, path, scale):
    def s16(v):
        return max(-32768, min(32767, int(round(v * scale))))

    # a triangle belongs to the joint its first vertex is bound to; segments
    # never straddle, because whole rings bind together
    per = {}
    for a, b, c, mat in rig.tris:
        per.setdefault((int(rig.bind_of[a]), mat), []).append((a, b, c))

    L = []
    w = L.append
    w("/* ph_rig.h - generated by ph_rig.py, do not edit")
    w(" *")
    w(" * Rigid segment rig for Dr. Patrick Horner: %d joints, %d tris."
      % (NJ, len(rig.tris)))
    w(" * Bind pose is the slumped standing mesh, so an all-zero pose is the")
    w(" * character exactly as the static header draws him.")
    w(" *")
    w(" * Vertices are stored in BIND space with the joint pivot already")
    w(" * subtracted, so drawing a segment is just: load its matrix, call its")
    w(" * display list.  Walk ph_joint_parent[] to build the matrix stack.")
    w(" */")
    w("#ifndef PH_RIG_H")
    w("#define PH_RIG_H")
    w("")
    w("#define PH_JOINT_COUNT %d" % NJ)
    w("")
    for i, n in enumerate(NAMES):
        w("#define PH_J_%-8s %2d" % (n.upper(), i))
    w("")
    w("static const s8 ph_joint_parent[PH_JOINT_COUNT] = {")
    w("    " + ", ".join("%d" % p for p in rig.parent))
    w("};")
    w("")
    w("/* local translation from parent pivot, s16 model units */")
    w("static const s16 ph_joint_local[PH_JOINT_COUNT][3] = {")
    for i, n in enumerate(NAMES):
        w("    {%6d,%6d,%6d},   /* %s */"
          % (s16(rig.local[i][0]), s16(rig.local[i][1]), s16(rig.local[i][2]),
             n))
    w("};")
    w("")

    dls = []
    for i, n in enumerate(NAMES):
        chunks = [(mat, tl) for (j, mat), tl in sorted(per.items())
                  if j == i]
        if not chunks:
            dls.append(None)
            continue
        vname = "ph_vtx_%s" % n
        body, flat = [], []
        for mat, tl in chunks:
            loads = batch(tl)
            if mat == "face":
                body.append("    gsDPSetCombineMode(G_CC_MODULATEIDECALA,"
                            " G_CC_MODULATEIDECALA),")
                body.append("    gsSPTexture(0x8000, 0x8000, 0,"
                            " G_TX_RENDERTILE, G_ON),")
            else:
                body.append("    gsSPTexture(0, 0, 0, 0, G_OFF),")
                body.append("    gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),")
            for vl, loc in loads:
                body.append("    gsSPVertex(&%s[%d], %d, 0),"
                            % (vname, len(flat), len(vl)))
                flat.extend(vl)
                k = 0
                while k + 1 < len(loc):
                    a, b = loc[k], loc[k + 1]
                    body.append("    gsSP2Triangles(%d,%d,%d, 0, %d,%d,%d, 0),"
                                % (a[0], a[1], a[2], b[0], b[1], b[2]))
                    k += 2
                if k < len(loc):
                    body.append("    gsSP1Triangle(%d,%d,%d, 0)," % loc[k])
        w("static const Vtx %s[] = {" % vname)
        for v in flat:
            x, y, z = rig.rest[v] - rig.pivot[i]
            u, t = rig.uv[v]
            r, g, b = rig.col[v]
            w("    {{{%6d,%6d,%6d}, 0, {%5d,%5d}, {%3d,%3d,%3d,255}}},"
              % (s16(x), s16(y), s16(z),
                 int(round(u * 31 * 32)), int(round(t * 31 * 32)), r, g, b))
        w("};")
        w("")
        w("static const Gfx ph_dl_%s[] = {" % n)
        for line in body:
            w(line)
        w("    gsSPEndDisplayList(),")
        w("};")
        w("")
        dls.append(n)

    w("/* NULL where a joint owns no geometry (pure pivots). */")
    w("static const Gfx *const ph_joint_dl[PH_JOINT_COUNT] = {")
    for i, n in enumerate(NAMES):
        w("    %-18s /* %s */" % (("ph_dl_%s," % n) if dls[i] else "NULL,", n))
    w("};")
    w("")
    w("#endif /* PH_RIG_H */")
    open(path, "w").write("\n".join(L) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scale", type=int, default=8)
    ap.add_argument("--outdir", default=".")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    rig = Rig()
    write_header(rig, os.path.join(a.outdir, "ph_rig.h"), a.scale)
    counts = {}
    for t in rig.tris:
        counts[NAMES[int(rig.bind_of[t[0]])]] = \
            counts.get(NAMES[int(rig.bind_of[t[0]])], 0) + 1
    print("joints %d   verts %d   tris %d" % (NJ, len(rig.rest), len(rig.tris)))
    for n in NAMES:
        print("   %-7s %3d tris" % (n, counts.get(n, 0)))


if __name__ == "__main__":
    main()
