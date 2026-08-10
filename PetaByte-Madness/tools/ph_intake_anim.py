#!/usr/bin/env python3
"""
ph_intake_anim.py — "intake": Horner gets onto the scanner and it takes him.

Fourteen and a half seconds, eight key poses, one root track and one prop
track.  The choreography is built around the holds rather than the moves:
he stands looking at it for nearly two seconds, sits on the end of the slab
for another two doing nothing at all, and the machine spends three and a
half seconds pulling him in.  The motion between those beats is deliberately
plain.  A tired man does not perform.

Two things worth knowing about the pose data:

  * Zero is the slumped standing pose, not a T-pose, so the hunch is the
    baseline and every key carries corrective angles away from it.  Lying
    supine he still does not straighten out - the neck and spine keys only
    give back about half the curl - which is the whole point.
  * Sitting on the END of the slab rather than its edge makes the lie-back a
    pure -90 pitch about X with no yaw component, so the transition reads
    cleanly at 15 fps without any intermediate keys.

Exports ph_anim_intake.h as keyframes plus a lerp note, NOT baked frames:
eight keys is 816 bytes, the same motion sampled per-frame at 20 fps is
29 KB, and the console can interpolate for free.

Usage:  python3 ph_intake_anim.py [--outdir .] [--fps 15] [--width 320]
"""

import argparse
import math
import os

import numpy as np

import dank_lab_gen as L
import ph_rig
from ph_rig import INDEX, NAMES, NJ, Rig
import lab_preview as LP

# --------------------------------------------------------------------------
# choreography
# --------------------------------------------------------------------------
DURATION = 14.5

# joint angles in degrees.  Upward segments (spine, neck, head) bend forward
# on +X; downward ones (thigh, shin, arm) swing forward on -X.
STAND = {}

SIT = {
    "hip_R": (-88, 0, 0), "hip_L": (-88, 0, 0),
    "kne_R": (84, 0, 0), "kne_L": (84, 0, 0),
    "ank_R": (-8, 0, 0), "ank_L": (-8, 0, 0),
    "spine": (4, 0, 0), "chest": (5, 0, 0),
    "neck": (3, 0, 0), "head": (6, 0, 0),
    "clav_R": (0, 0, -6), "clav_L": (0, 0, 6),
}

RECLINE = {                       # halfway back, legs still coming up
    "hip_R": (-36, 0, 0), "hip_L": (-36, 0, 0),
    "kne_R": (34, 0, 0), "kne_L": (34, 0, 0),
    "spine": (1, 0, 0), "chest": (1, 0, 0),
    "neck": (-4, 0, 0), "head": (-2, 0, 0),
    "clav_R": (0, 0, -8), "clav_L": (0, 0, 8),
}

LIE = {                           # only about half the curl comes back out
    "spine": (-3, 0, 0), "chest": (-4, 0, 0),
    "neck": (-9, 0, 0), "head": (-7, 0, 0),
    "ank_R": (-6, 0, 0), "ank_L": (-6, 0, 0),
    "clav_R": (0, 0, -5), "clav_L": (0, 0, 5),
}

POSE_KEYS = [
    (0.00, STAND), (1.80, STAND), (3.00, STAND),
    (4.40, SIT), (6.20, SIT),
    (7.40, RECLINE), (8.40, LIE), (DURATION, LIE),
]

# root pivot world position, then yaw and pitch in degrees
ROOT_KEYS = [
    (0.00, (-70.0, 96.0, 158.0), 180.0, 0.0),   # facing it
    (1.80, (-70.0, 96.0, 158.0), 180.0, 0.0),
    (3.00, (-70.0, 96.0, 152.0), 0.0, 0.0),     # turns his back on it
    (4.40, (-70.0, 97.0, 108.0), 0.0, 0.0),     # sits on the end
    (6.20, (-70.0, 97.0, 108.0), 0.0, 0.0),
    (7.40, (-70.0, 99.0, 66.0), 0.0, -44.0),
    (8.40, (-70.0, 100.0, 24.0), 0.0, -90.0),   # supine, head toward the bore
    (DURATION, (-70.0, 100.0, 24.0), 0.0, -90.0),
]

# the slab's own track: rest, then 150 cm into the bore
TABLE_KEYS = [(0.00, 0.0), (9.60, 0.0), (13.00, -150.0), (DURATION, -150.0)]

CAM_KEYS = [
    (0.00, (146.0, 156.0, 214.0), (-62.0, 106.0, 76.0)),
    (4.40, (132.0, 150.0, 200.0), (-66.0, 102.0, 62.0)),
    (8.40, (118.0, 142.0, 182.0), (-70.0, 100.0, 30.0)),
    (11.50, (96.0, 134.0, 160.0), (-72.0, 100.0, -34.0)),
    (DURATION, (74.0, 128.0, 138.0), (-74.0, 102.0, -86.0)),
]


def ease(t):
    """Smoothstep.  Linear keys on a body this heavy look mechanical."""
    return t * t * (3.0 - 2.0 * t)


def _span(keys, t):
    for i in range(len(keys) - 1):
        if keys[i][0] <= t <= keys[i + 1][0]:
            a, b = keys[i], keys[i + 1]
            span = b[0] - a[0]
            return i, (ease((t - a[0]) / span) if span > 1e-6 else 0.0)
    return len(keys) - 2, 1.0


def pose_at(t):
    i, u = _span(POSE_KEYS, t)
    a, b = POSE_KEYS[i][1], POSE_KEYS[i + 1][1]
    out = np.zeros((NJ, 3))
    for n in set(a) | set(b):
        va = np.array(a.get(n, (0, 0, 0)), float)
        vb = np.array(b.get(n, (0, 0, 0)), float)
        out[INDEX[n]] = va + (vb - va) * u
    return out


def root_at(t, ride=True):
    """Root keys are expressed in the SLAB's frame from the moment he is on
    it, so once the drive engages he has to be parented to the prop track or
    the machine slides out from under him.  Runtime does the same: add
    ph_intake_table's dz to the root translation from PH_INTAKE_CUE_LIE on."""
    i, u = _span(ROOT_KEYS, t)
    a, b = ROOT_KEYS[i], ROOT_KEYS[i + 1]
    pos = np.array(a[1]) + (np.array(b[1]) - np.array(a[1])) * u
    yaw = a[2] + (b[2] - a[2]) * u
    pitch = a[3] + (b[3] - a[3]) * u
    if ride:
        pos = pos + np.array([0.0, 0.0, table_at(t)])
    return pos, yaw, pitch


def table_at(t):
    i, u = _span(TABLE_KEYS, t)
    a, b = TABLE_KEYS[i], TABLE_KEYS[i + 1]
    return a[1] + (b[1] - a[1]) * u


def cam_at(t):
    i, u = _span(CAM_KEYS, t)
    a, b = CAM_KEYS[i], CAM_KEYS[i + 1]
    eye = np.array(a[1]) + (np.array(b[1]) - np.array(a[1])) * u
    tgt = np.array(a[2]) + (np.array(b[2]) - np.array(a[2])) * u
    return eye, tgt


# --------------------------------------------------------------------------
# N64 keyframe export
# --------------------------------------------------------------------------
def s16ang(deg):
    """Binary angle: 0x4000 == 90 degrees, the usual N64 convention."""
    v = int(round(deg / 360.0 * 65536.0)) & 0xFFFF
    return v - 65536 if v > 32767 else v


def write_header(path, scale, fps):
    def u(v):
        return max(-32768, min(32767, int(round(v * scale))))

    L_ = []
    w = L_.append
    w("/* ph_anim_intake.h - generated by ph_intake_anim.py, do not edit")
    w(" *")
    w(" * \"intake\": Horner sits on the scanner slab and the machine draws")
    w(" * him in.  %.1f s, %d pose keys, %d root keys, %d prop keys."
      % (DURATION, len(POSE_KEYS), len(ROOT_KEYS), len(TABLE_KEYS)))
    w(" *")
    w(" * KEYS, not baked frames.  Lerp between them at whatever rate you")
    w(" * run at, and smoothstep the parameter - linear on a body this heavy")
    w(" * reads mechanical.  Baking this out per-frame at 20 fps would cost")
    w(" * ~29 KB; the keys below are under 1 KB.")
    w(" *")
    w(" * Angles are binary: 0x4000 == 90 degrees.  Order is Z, then Y,")
    w(" * then X, matching euler() in ph_rig.py.")
    w(" * Zero is the slumped standing pose, NOT a rest pose.")
    w(" *")
    w(" * PARENTING: from PH_INTAKE_CUE_LIE onward the root translation is")
    w(" * in the SLAB's frame - add ph_intake_table's interpolated dz to it,")
    w(" * or the machine slides out from under him.")
    w(" */")
    w("#ifndef PH_ANIM_INTAKE_H")
    w("#define PH_ANIM_INTAKE_H")
    w("")
    w("#include \"ph_rig.h\"")
    w("")
    w("#define PH_INTAKE_FPS        %d" % fps)
    w("#define PH_INTAKE_FRAMES     %d" % int(round(DURATION * fps)))
    w("#define PH_INTAKE_POSE_KEYS  %d" % len(POSE_KEYS))
    w("#define PH_INTAKE_ROOT_KEYS  %d" % len(ROOT_KEYS))
    w("#define PH_INTAKE_TABLE_KEYS %d" % len(TABLE_KEYS))
    w("")
    w("typedef struct { u16 frame; s16 rot[PH_JOINT_COUNT][3]; } PhPoseKey;")
    w("typedef struct { u16 frame; s16 pos[3]; s16 yaw, pitch; } PhRootKey;")
    w("typedef struct { u16 frame; s16 dz; } PhPropKey;")
    w("")
    w("static const PhPoseKey ph_intake_pose[PH_INTAKE_POSE_KEYS] = {")
    for t, _ in POSE_KEYS:
        p = pose_at(t + 1e-6 if t < DURATION else t)
        w("    { %3d, {" % int(round(t * fps)))
        for j in range(NJ):
            w("        {%7d,%7d,%7d},   /* %s */"
              % (s16ang(p[j][0]), s16ang(p[j][1]), s16ang(p[j][2]), NAMES[j]))
        w("    }},")
    w("};")
    w("")
    w("static const PhRootKey ph_intake_root[PH_INTAKE_ROOT_KEYS] = {")
    for t, pos, yaw, pitch in ROOT_KEYS:
        w("    { %3d, {%6d,%6d,%6d}, %7d, %7d },"
          % (int(round(t * fps)), u(pos[0]), u(pos[1]), u(pos[2]),
             s16ang(yaw), s16ang(pitch)))
    w("};")
    w("")
    w("/* Slide the lab's \"table\" segment by this along Z.  It is a separate")
    w(" * group in dank_lab_gen.py precisely so it can move on its own. */")
    w("static const PhPropKey ph_intake_table[PH_INTAKE_TABLE_KEYS] = {")
    for t, dz in TABLE_KEYS:
        w("    { %3d, %6d }," % (int(round(t * fps)), u(dz)))
    w("};")
    w("")
    w("/* Hold points worth hitting exactly if you cut sound to this: */")
    w("#define PH_INTAKE_CUE_LOOK   %d   /* he is just standing there */"
      % int(round(0.0 * fps)))
    w("#define PH_INTAKE_CUE_SIT    %d   /* weight lands on the slab */"
      % int(round(4.40 * fps)))
    w("#define PH_INTAKE_CUE_LIE    %d   /* head touches down */"
      % int(round(8.40 * fps)))
    w("#define PH_INTAKE_CUE_MOTOR  %d   /* slab drive starts */"
      % int(round(9.60 * fps)))
    w("#define PH_INTAKE_CUE_IN     %d   /* he is inside */"
      % int(round(13.00 * fps)))
    w("")
    w("#endif /* PH_ANIM_INTAKE_H */")
    open(path, "w").write("\n".join(L_) + "\n")


# --------------------------------------------------------------------------
# preview render
# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default=".")
    ap.add_argument("--scale", type=int, default=8)
    ap.add_argument("--fps", type=int, default=15)
    ap.add_argument("--width", type=int, default=320)
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    W, H = a.width, int(a.width * 3 / 4)

    rig = Rig()
    L.POS.clear(); L.COL.clear(); L.UVS.clear()
    L.TRIS.clear(); L.EMISSIVE.clear()
    L.build()

    lab_p = np.array(L.POS, float)
    lab_c = np.array(L.COL, float)
    lab_uv = np.array(L.UVS, float)
    room = [(t[0], t[1], t[2], t[3], t[4]) for t in L.TRIS if t[5] == "room"]
    slab = [t for t in room if t[4] == "table"]
    fixed = [t for t in room if t[4] != "table"]
    slab_v = sorted({v for t in slab for v in t[:3]})
    slab_ix = {v: k for k, v in enumerate(slab_v)}
    bore = [i for i in L.EMISSIVE if L.COL[i] == L.E_BORE]

    tex = {}
    for mat, name in (("wall", "lab_wall"), ("floor", "lab_floor"),
                      ("screen", "lab_screen"), ("caustic", "lab_caustic")):
        tex[mat] = LP.load_tex(a.outdir, name)
    tex["hface"] = LP.load_tex(a.outdir, "ph_face")
    tint = np.array([0.42, 0.52, 0.54])
    ph_col = np.array([[c * k for c, k in zip(col, tint)]
                       for col in rig.col], float)

    class Scene:
        pass

    nframes = int(round(DURATION * a.fps))
    frames = []
    for f in range(nframes):
        t = f / a.fps
        dz = table_at(t)
        pos, yaw, pitch = root_at(t)
        seg, idx = rig.pose_segments(pose_at(t), pos, yaw, pitch)

        P = [lab_p, lab_p[slab_v] + np.array([0.0, 0.0, dz]), seg]
        C = [lab_c, lab_c[slab_v], ph_col[idx]]
        U = [lab_uv, lab_uv[slab_v], np.array([rig.uv[i] for i in idx])]

        # the bore lamp winds up as the drive engages
        glow = 1.0 + 0.55 * max(0.0, min(1.0, (t - 9.0) / 2.2)) \
            + 0.12 * math.sin(t * 7.0) * (t > 9.6)
        cc = C[0].copy()
        cc[bore] = np.clip(cc[bore] * glow, 0, 255)
        C[0] = cc

        sc = Scene()
        sc.P = np.concatenate(P)
        sc.C = np.concatenate(C)
        sc.UV = np.concatenate(U)
        o1 = len(lab_p)
        o2 = o1 + len(slab_v)
        sc.T = [(x, y, z, m) for x, y, z, m, _ in fixed]
        sc.T += [(o1 + slab_ix[x], o1 + slab_ix[y], o1 + slab_ix[z], m)
                 for x, y, z, m, _ in slab]
        sc.T += [(o2 + 3 * k, o2 + 3 * k + 1, o2 + 3 * k + 2,
                  "hface" if rig.tris[k][3] == "face" else "shade")
                 for k in range(len(rig.tris))]

        eye, tgt = cam_at(t)
        frames.append(LP.render(sc, tex, W, H, eye, tgt))
        if f % 30 == 0:
            print("  frame %d/%d" % (f, nframes), flush=True)

    import imageio.v2 as imageio
    from PIL import Image
    gif = os.path.join(a.outdir, "horner_intake.gif")
    mp4 = os.path.join(a.outdir, "horner_intake.mp4")
    # The GIF is a review artefact.  Nearest-neighbour downscale and a
    # shared 96-colour palette: bilinear would invent gradients across the
    # flat shaded facets and triple the file for no extra information.
    small = [Image.fromarray(f).resize((W * 3 // 4, H * 3 // 4), Image.NEAREST)
             .convert("P", palette=Image.ADAPTIVE, colors=96)
             for f in frames]
    small[0].save(gif, save_all=True, append_images=small[1:],
                  duration=int(1000 / a.fps), loop=0, optimize=True)
    imageio.mimsave(mp4, frames, fps=a.fps, quality=8, macro_block_size=1)

    write_header(os.path.join(a.outdir, "ph_anim_intake.h"), a.scale, a.fps)
    print("%d frames at %d fps -> %s, %s" % (nframes, a.fps, gif, mp4))


if __name__ == "__main__":
    main()
