#!/usr/bin/env python3
"""
mc_head_preview.py — tight, auto-framed renders of the head only.

The full-body preview cameras are useless for judging a face: the head is
forty pixels tall and half of it is behind the shoulder.  This finds the
bounding box of the head/face/optic groups, parks a camera at a fixed radius
around it, and renders the angles that actually decide whether the read works
— dead front, both three-quarters, and profile from his bone side.

Usage: python3 mc_head_preview.py [obj] [--fill 0.55]
"""
import math
import sys

import numpy as np
from PIL import Image

import mc_preview as P

HEAD_GROUPS = {"head", "face", "optic"}


def is_head(group):
    """Group names arrive as "<bone>__<material>"; mc_preview hands back only
    the material half, and head/face/optic are unique to the head either way."""
    return group in HEAD_GROUPS or group.split("__")[0] == "head"


def head_bbox(V, F):
    idx = sorted({i for (a, b, c, _, g) in F if is_head(g)
                  for i in (a, b, c)})
    pts = V[idx]
    return pts.min(axis=0), pts.max(axis=0)


def orbit(center, radius, yaw_deg, pitch_deg):
    """Camera position on a sphere.  yaw 0 = dead front (+Z), + = toward his left."""
    ya, pa = math.radians(yaw_deg), math.radians(pitch_deg)
    return (center[0] + radius * math.sin(ya) * math.cos(pa),
            center[1] + radius * math.sin(pa),
            center[2] + radius * math.cos(ya) * math.cos(pa))


def main():
    src = "out/machine_centaur.obj"
    fill = 0.55
    args = sys.argv[1:]
    if args and not args[0].startswith("--"):
        src = args.pop(0)
    if "--fill" in args:
        fill = float(args[args.index("--fill") + 1])

    V, C, T, F, tex = P.load_obj(src)
    lo, hi = head_bbox(V, F)
    center = (lo + hi) * 0.5
    span = float(np.max(hi - lo))
    radius = span * 2.4

    # lift the ambient floor so the bone/steel split is judgeable; the console
    # will light this properly, the point here is to read the shapes
    Cb = np.clip(C * (1.0 + fill), 0, 1)

    shots = [
        ("front",   0,    4),
        ("q_right", -38,  6),   # his right — the bone side
        ("q_left",   38,  6),   # his left  — the plate and both lenses
        ("side_r",  -84,  2),
        ("low",      -14, -16),  # from below, which is how you meet him
    ]
    for name, yaw, pitch in shots:
        eye = orbit(center, radius, yaw, pitch)
        P.render(V, Cb, T, F, tex, eye, tuple(center), fov=34,
                 path="out/head_%s.png" % name)

    print("head bbox  X %.1f..%.1f  Y %.1f..%.1f  Z %.1f..%.1f  (span %.1f cm)"
          % (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], span))
    print("wrote " + ", ".join("out/head_%s.png" % s[0] for s in shots))


if __name__ == "__main__":
    main()
