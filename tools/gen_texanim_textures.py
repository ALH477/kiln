#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""gen_texanim_textures.py — texanim-demo's own textures, on gen_textures' noise.

Run by nix/assets.nix's mkTextures as an extra generator, after gen_textures.py,
into the same directory, with gen_textures importable. Kept out of TEXTURES on
purpose: eight flipbook frames are 16 KB of RGBA16, and every ROM that ships
the shared texture set would carry them.

  fire0..7.rgba16.png   a flame, one frame per 4 px of upward scroll. The noise
                        wraps at 32 px, so frame 8 would be frame 0 exactly:
                        the loop has no seam, which a painted flipbook rarely
                        manages
  sky.rgba16.png        an environment for the chrome ball: sky over a horizon
                        over ground, with a sun. SPHERICAL_UV maps view-space
                        normals into this square, so it reads as a reflection
                        only if it looks like surroundings — the grid it
                        replaces read as a wobbling grid

Deterministic, like gen_textures: no random, no timestamps.

Usage: gen_texanim_textures.py <outdir>
"""

import sys
from pathlib import Path

import gen_textures as g

SIZE = g.SIZE
FIRE_FRAMES = 8


def _clamp(v):
    return 0 if v < 0 else (255 if v > 255 else int(v))


def _fire_colour(h):
    """Heat 0..1 to black-body-ish colour: black, red, orange, yellow, white."""
    stops = ((0.00, (0, 0, 0)), (0.30, (120, 10, 0)), (0.55, (230, 70, 0)),
             (0.78, (255, 180, 30)), (1.00, (255, 250, 220)))
    for (h0, c0), (h1, c1) in zip(stops, stops[1:]):
        if h <= h1:
            t = (h - h0) / (h1 - h0) if h1 > h0 else 0.0
            return tuple(_clamp(c0[k] + (c1[k] - c0[k]) * t) for k in range(3))
    return stops[-1][1]


def tex_fire(size, frame):
    px = bytearray()
    scroll = frame * size // FIRE_FRAMES
    for y in range(size):
        # Row 0 is the top of the image; flames rise from the bottom.
        up = 1.0 - y / (size - 1)
        for x in range(size):
            n = g.fbm(x, (y + scroll) % size, size, seed=41, octaves=3)
            # Hot at the base, thinning towards the top, broken up by the noise.
            heat = n * 1.35 - up * 0.95 + 0.25
            heat = 0.0 if heat < 0.0 else (1.0 if heat > 1.0 else heat)
            r, gg, b = _fire_colour(heat)
            px += bytes((r, gg, b, 0xFF))
    return px


def tex_sky(size):
    """Upside down on purpose: Tiny3D's SPHERICAL_UV puts an UP-facing normal
    on the texture's BOTTOM rows. Drawn the natural way up, the chrome ball
    reflected the ground above it and the sky below (first Ares capture)."""
    px = bytearray()
    horizon = size * 0.56
    sun = (size * 0.30, size * 0.26)
    for row in range(size):
        y = size - 1 - row
        for x in range(size):
            if y < horizon:
                t = y / horizon
                r, gg, b = 40 + 170 * t, 90 + 140 * t, 200 + 50 * t
                cloud = g.fbm(x, y, size, seed=7, octaves=2)
                if cloud > 0.62:
                    k = min(1.0, (cloud - 0.62) * 4.0)
                    r, gg, b = r + (245 - r) * k, gg + (245 - gg) * k, b + (250 - b) * k
                d2 = (x - sun[0]) ** 2 + (y - sun[1]) ** 2
                if d2 < 12:
                    r, gg, b = 255, 245, 200
            else:
                t = (y - horizon) / (size - horizon)
                n = g.fbm(x, y, size, seed=19, octaves=2)
                r, gg, b = 110 - 60 * t + 30 * n, 84 - 40 * t + 20 * n, 52 - 20 * t
            px += bytes((_clamp(r), _clamp(gg), _clamp(b), 0xFF))
    return px


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    out = Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for f in range(FIRE_FRAMES):
        path = out / f"fire{f}.rgba16.png"
        g.write_png(path, SIZE, SIZE, tex_fire(SIZE, f))
        print(f"  [PNG] {path.name}  {SIZE}x{SIZE}")
    path = out / "sky.rgba16.png"
    g.write_png(path, SIZE, SIZE, tex_sky(SIZE))
    print(f"  [PNG] {path.name}  {SIZE}x{SIZE}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
