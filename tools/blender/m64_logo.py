# SPDX-License-Identifier: MPL-2.0
"""m64_logo.py — the M64 boot logo, as a 3D model.

    blender --background --factory-startup -noaudio --python m64_logo.py \
        -- --model m64_logo --out build/m64_logo.gltf

A parody of the Nintendo 64 boot, so it is built the way that logo was: a
chunky extruded wordmark with real depth, assembled out of axis-aligned
slabs rather than drawn as a texture. The N64's logo spun a solid "N" in
three dimensions, and the thing that made it read on a CRT at 240 lines was
that it had thickness and caught the light differently on each face.

Two objects, exported separately so the splash can move them
independently:

  `m64`    the M64 wordmark — the piece that spins
  `demod`  a plate that sits behind it and does not

Everything is boxes. `m64lib.box` splits corners per face, so each face
keeps its own flat normal and its own colour — which is exactly what a
faceted logo wants, and why the letters read as solid rather than as a soft
grey lump under the engine's single directional light.

── The glyphs ─────────────────────────────────────────────────────────────
An M is four slabs (two uprights and two diagonals-as-steps), a 6 and a 4
are five and four. Building letters from rectangles rather than from a
font mesh keeps the whole thing under 400 triangles and gives it the
blocky, slightly wrong-looking geometry a 1996 logo actually had.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m64lib as m  # noqa: E402

# Stroke thickness and depth, in Blender units (metres). The logo is
# authored about 1 unit tall so the splash can scale it in one place.
S = 0.16   # stroke width
D = 0.30   # extrusion depth
H = 1.00   # cap height

# Deliberately not the N64's red. DeMoD's mark reads cold; the accent is
# the same crimson the veil uses, so the boot and the game agree on what
# red means in this product.
FACE = m.srgb(206, 202, 196)
SIDE = m.srgb(128, 126, 122)
ACCENT = m.srgb(176, 26, 32)


def slab(parts, cx, cy, sx, sy, color):
    """One extruded rectangle on the logo plane, added to `parts`."""
    verts, quads = m.box(cx, 0.0, cy, sx, D, sy)
    base = len(parts["verts"])
    parts["verts"].extend(verts)
    parts["faces"].extend(tuple(base + i for i in q) for q in quads)
    # Per-face colour: the two faces normal to Y are the front and back
    # caps and get the bright value; the four sides get the darker one, so
    # the extrusion reads even when the light is flat on it.
    for n, _q in enumerate(quads):
        parts["colors"].append(color if n < 2 else SIDE)


def glyph_M(parts, x0, color):
    """M — two uprights and two inward steps. The steps are slabs rather
    than true diagonals because a rotated quad would need its own normals
    and buys nothing at the size this is ever seen."""
    w = 0.86
    slab(parts, x0, H / 2, S, H, color)                    # left upright
    slab(parts, x0 + w, H / 2, S, H, color)                # right upright
    slab(parts, x0 + w * 0.30, H * 0.74, S * 0.9, H * 0.52, color)
    slab(parts, x0 + w * 0.70, H * 0.74, S * 0.9, H * 0.52, color)
    slab(parts, x0 + w * 0.50, H * 0.90, S * 0.9, H * 0.22, color)


def glyph_6(parts, x0, color):
    w = 0.66
    slab(parts, x0, H / 2, S, H, color)                    # spine
    slab(parts, x0 + w / 2, H - S / 2, w, S, color)        # top
    slab(parts, x0 + w / 2, H / 2, w, S, color)            # waist
    slab(parts, x0 + w / 2, S / 2, w, S, color)            # bottom
    slab(parts, x0 + w, H * 0.25, S, H * 0.5, color)       # lower right


def glyph_4(parts, x0, color):
    w = 0.66
    slab(parts, x0, H * 0.72, S, H * 0.56, color)          # upper left
    slab(parts, x0 + w / 2, H * 0.44, w + S, S, color)     # crossbar
    slab(parts, x0 + w, H / 2, S, H, color)                # stem


def build_wordmark():
    parts = {"verts": [], "faces": [], "colors": []}
    glyph_M(parts, 0.00, FACE)
    glyph_6(parts, 1.28, ACCENT)
    glyph_4(parts, 2.20, ACCENT)

    # Centre it on X so the splash can spin it about its own middle rather
    # than about its left edge — a logo that orbits instead of rotating is
    # the classic tell of a pivot left at the origin.
    xs = [v[0] for v in parts["verts"]]
    cx = (min(xs) + max(xs)) / 2.0
    parts["verts"] = [(v[0] - cx, v[1], v[2]) for v in parts["verts"]]

    m.make_material("m64")
    m.make_mesh("m64", parts["verts"], parts["faces"], "m64",
                colors=parts["colors"], smooth=False)


def build_plate():
    """A thin bar under the wordmark. The splash fades it up late, with
    the DeMoD text drawn over it in the 2D pass — text stays 2D because a
    3D "Made by DeMoD LLC" would need a font mesh for one screen."""
    parts = {"verts": [], "faces": [], "colors": []}
    slab(parts, 0.0, -0.28, 3.10, 0.05, ACCENT)
    m.make_material("demod")
    m.make_mesh("demod", parts["verts"], parts["faces"], "demod",
                colors=parts["colors"], smooth=False)


def main():
    m.reset_scene()
    build_wordmark()
    build_plate()
    m.report()
    m.export_gltf(m.arg("--out"))


main()
