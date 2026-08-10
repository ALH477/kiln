#!/usr/bin/env python3
"""
mc_anim_render.py — pose the rig and encode the result.

This imports machine_centaur_gen directly rather than reading the OBJ, so the
keyframes it plays are literally the ones emitted into machine_centaur.h.  A
separately hand-tuned preview would be worthless: the whole point of the video
is to show what the console will do, and the only way to guarantee that is for
both to read the same tables.

Rigid binding, so posing is one 4x4 per vertex — no weights, no blending, the
same thing the matrix stack does on hardware.

Usage:  python3 mc_anim_render.py [--fps 20] [--width 420] [--outdir out]
"""
import argparse
import math
import os
import subprocess

import numpy as np
from PIL import Image

import machine_centaur_gen as G
import mc_preview as P

# Which camera suits which animation.  A walk cycle is unreadable head-on and a
# fire needs to be side-on enough to see the barrel move.
UPPER = ("torso", "head", "jaw", "face", "armL", "armR", "gun",
         "claw0", "claw1", "claw2")

SHOTS = {
    "idle":   dict(yaw=34, pitch=8, tgt=0.50),
    "walk":   dict(yaw=-64, pitch=6,  dist=1.60, tgt=0.46),
    "fire":   dict(yaw=42, pitch=7, tgt=0.50, margin=1.10),
    # notice is about the head and the gun coming up, so it frames only the
    # human half — fitting the whole leg span pushes the camera seven metres
    # out and throws away the one thing the animation is for
    "notice": dict(yaw=-26, pitch=2, tgt=0.52, margin=1.06,
                   focus=("torso", "head", "armL", "armR", "gun")),
    # the claw work is all upper body — framing the leg span throws away the
    # hand, which is the only thing these four animations are about
    "grab":   dict(yaw=54, pitch=6, tgt=0.50, margin=1.10,
                   focus=UPPER),
    "slash":  dict(yaw=46, pitch=8, tgt=0.48, margin=1.12,
                   focus=UPPER),
    "combo_slash_shoot": dict(yaw=48, pitch=8, tgt=0.48, margin=1.12,
                              focus=UPPER),
    "combo_shoot_slash": dict(yaw=48, pitch=8, tgt=0.48, margin=1.12,
                              focus=UPPER),
    # these two are about the chassis as much as the arm, so they get the whole
    # animal in frame — the point is watching the weight go somewhere
    "slash_overhand": dict(yaw=62, pitch=8, tgt=0.46, margin=1.06),
    "punch_right":    dict(yaw=-58, pitch=8, tgt=0.46, margin=1.06),
    "jump":   dict(yaw=54, pitch=6, tgt=0.46, margin=1.04),
    # talking is a face performance; frame the head or it is 40 pixels of jaw
    "talk":   dict(yaw=18, pitch=2, tgt=0.86, margin=1.30,
                   focus=("head", "jaw", "face", "optic")),
    "shout":  dict(yaw=26, pitch=2, tgt=0.70, margin=1.15,
                   focus=("head", "jaw", "face", "optic", "torso")),
}


def build_model():
    """Run the generator's build passes without touching the disk emitters."""
    G.build_bones()
    G.ANIMS = G.build_anims()
    with G.bone("hull"):
        G.build_hull()
    for side in (1, -1):
        for pair in ("front", "rear"):
            G.build_leg(side, pair)
    human = len(G.M.v)
    with G.bone("torso"):
        seam = G.build_graft()
        G.build_torso(seam)
        G.build_spine()
        G.build_head()
        G.build_right_arm()
        G.build_shotgun_arm()
    for i in range(human, len(G.M.v)):
        x, y, z = G.M.v[i]
        G.M.v[i] = (x, y, z + G.lean_z(y))


def to_preview_arrays(outdir):
    """Flatten the mesh into the (V, C, T, F, tex) tuple mc_preview renders."""
    V = np.array(G.M.v, dtype=float)
    C = np.array([[c / 255.0 for c in col] for col in G.M.c])
    T = np.array([[u, 1.0 - v] for (u, v) in G.M.uv])
    tex = {}
    for name, path in (("face", "mc_face.png"), ("hull", "mc_plate.png"),
                       ("gore", "mc_gore.png")):
        f = os.path.join(outdir, path)
        if os.path.exists(f):
            tex[name] = np.asarray(Image.open(f).convert("RGB"),
                                   dtype=np.float64) / 255.0
    F = []
    for (bn, mat), tris in G.M.groups.items():
        for (a, b, c) in tris:
            F.append((a, b, c, (a, b, c), mat))
    return V, C, T, F, tex


def pose(V, bones, mats):
    """One matrix per vertex.  Rigid binding makes this a gather, not a solve."""
    out = np.empty_like(V)
    hom = np.hstack([V, np.ones((len(V), 1))])
    for name, Mx in mats.items():
        sel = bones == name
        if sel.any():
            out[sel] = (Mx @ hom[sel].T).T[:, :3]
    return out


def encode(frames, base, fps):
    """GIF for anywhere it gets pasted, MP4 for anywhere that respects it."""
    made = []
    gif = base + ".gif"
    pal = [f.convert("P", palette=Image.ADAPTIVE, colors=128) for f in frames]
    pal[0].save(gif, save_all=True, append_images=pal[1:],
                duration=int(1000 / fps), loop=0, optimize=False, disposal=2)
    made.append(gif)

    mp4 = base + ".mp4"
    raw = base + "_%04d.png"
    for i, im in enumerate(frames):
        im.save(raw % i)
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(fps),
           "-i", raw, "-c:v", "libx264", "-pix_fmt", "yuv420p",
           "-crf", "18", "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2", mp4]
    if subprocess.call(cmd) == 0:
        made.append(mp4)
    for i in range(len(frames)):
        os.remove(raw % i)
    return made


def fit_distance(pts, ctr, yaw, pitch, fov, W, H, margin=1.10):
    """Smallest camera distance that keeps every point on screen.

    Hand-tuned distances were wrong for three of the four shots and would go
    wrong again the moment a limb's amplitude changed, so solve it instead:
    project the pose cloud onto the camera's own right/up axes and take the
    distance each extreme needs, then keep the largest.
    """
    ya, pa = math.radians(yaw), math.radians(pitch)
    fwd = -np.array([math.sin(ya) * math.cos(pa), math.sin(pa),
                     math.cos(ya) * math.cos(pa)])
    right = np.cross(fwd, [0, 1, 0])
    right /= np.linalg.norm(right)
    up = np.cross(right, fwd)

    d = pts - ctr
    ty = math.tan(math.radians(fov) * 0.5)          # vertical half-angle
    tx = ty * (W / float(H))
    need = 1.0
    for axis, t in ((right, tx), (up, ty)):
        lat = np.abs(d @ axis)
        depth = d @ fwd                             # + is toward the subject
        need = max(need, float(np.max(lat / t + depth)))
    return need * margin


def make_ground(V, extent=300.0, n=14):
    """A floor, for the preview only — it is not part of the model.

    A walk cycle rendered against black is unreadable: with nothing to
    reference, a foot that lifts and a foot that stays put look identical, and
    every timing error hides.  The grid gives the stride something to measure
    against and the blob under the hull gives the feet somewhere to land.
    """
    GV, GC, GF = [], [], []
    base = len(V)
    step = 2.0 * extent / n
    for i in range(n):
        for j in range(n):
            x0, z0 = -extent + i * step, -extent + j * step
            k = 0.115 + 0.028 * ((i + j) % 2)
            idx = len(GV) + base
            for (dx, dz) in ((0, 0), (step, 0), (step, step), (0, step)):
                GV.append([x0 + dx, 0.0, z0 + dz])
                GC.append([k, k, k * 1.06])
            GF.append((idx, idx + 1, idx + 2, (-1, -1, -1), "ground"))
            GF.append((idx, idx + 2, idx + 3, (-1, -1, -1), "ground"))

    # contact shadow: a flat disc riding just above the deck under the chassis
    cx = len(GV) + base
    GV.append([0.0, 0.35, -8.0])
    GC.append([0.045, 0.045, 0.05])
    for k in range(20):
        a = 2.0 * math.pi * k / 20.0
        GV.append([132.0 * math.cos(a), 0.35, -8.0 + 96.0 * math.sin(a)])
        GC.append([0.075, 0.075, 0.082])
    for k in range(20):
        GF.append((cx, cx + 1 + k, cx + 1 + (k + 1) % 20,
                   (-1, -1, -1), "ground"))
    return np.array(GV), np.array(GC), GF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fps", type=int, default=20)
    ap.add_argument("--width", type=int, default=420)
    ap.add_argument("--outdir", default="out")
    ap.add_argument("--only", default=None)
    ap.add_argument("--no-ground", action="store_true")
    args = ap.parse_args()

    P.W = args.width
    P.H = int(args.width * 0.70)        # he is wide and low; frame him that way

    build_model()
    V, C, T, F, tex = to_preview_arrays(args.outdir)
    bones = np.array(G.M.b)
    C = np.clip(C * 1.34, 0, 1)         # lift the ambient floor for preview

    GV, GC, GF = make_ground(V)
    if args.no_ground:
        GV, GC, GF = np.zeros((0, 3)), np.zeros((0, 3)), []

    made = []
    for name, anim in G.ANIMS.items():
        if args.only and name != args.only:
            continue
        s = SHOTS.get(name, SHOTS["idle"])

        n = anim["frames"] if anim["loop"] else anim["frames"] + 1
        step = max(1, round(n / 48.0))              # cap at ~48 rendered frames
        poses = [pose(V, bones, G.pose_matrices(anim, f))
                 for f in range(0, n, step)]

        # frame against every pose in the clip, not the rest pose, so a raised
        # gun or a lifted foot cannot walk out of shot mid-cycle
        if "focus" in s:
            sel = np.isin(bones, s["focus"])
            cloud = np.vstack([p[sel] for p in poses])
        else:
            cloud = np.vstack(poses)
        lo, hi = cloud.min(axis=0), cloud.max(axis=0)
        ctr = np.array([(lo[0] + hi[0]) * 0.5,
                        lo[1] + (hi[1] - lo[1]) * s["tgt"],
                        (lo[2] + hi[2]) * 0.5])
        r = fit_distance(cloud[::7], ctr, s["yaw"], s["pitch"], 36,
                         P.W, P.H, s.get("margin", 1.08))
        ya, pa = math.radians(s["yaw"]), math.radians(s["pitch"])
        eye = (ctr[0] + r * math.sin(ya) * math.cos(pa),
               ctr[1] + r * math.sin(pa),
               ctr[2] + r * math.cos(ya) * math.cos(pa))

        Call = np.vstack([C, GC]) if len(GC) else C
        frames = []
        for Vp in poses:
            Vall = np.vstack([Vp, GV]) if len(GV) else Vp
            path = os.path.join(args.outdir, "_f.png")
            P.render(Vall, Call, T, F + GF, tex, eye, tuple(ctr), fov=36,
                     path=path)
            frames.append(Image.open(path).convert("RGB").copy())
        if not anim["loop"]:                        # hold the last pose a beat
            frames += [frames[-1]] * max(2, args.fps // 3)
        made += encode(frames, os.path.join(args.outdir, "anim_" + name),
                       args.fps)
        print("%-7s %2d frames  cam %.0f cm  -> %s"
              % (name, len(frames), r,
                 ", ".join(os.path.basename(m) for m in made[-2:])))
    stray = os.path.join(args.outdir, "_f.png")
    if os.path.exists(stray):
        os.remove(stray)


if __name__ == "__main__":
    main()
