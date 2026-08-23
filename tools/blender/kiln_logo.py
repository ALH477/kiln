# SPDX-License-Identifier: MIT
"""kiln_logo.py — the Kiln boot logo, as a 3D model.

    blender --background --factory-startup -noaudio --python kiln_logo.py \
        -- --model kiln_logo --out build/kiln_logo.gltf

A parody of the Nintendo 64 boot, so it is built the way that logo was: a
chunky extruded wordmark with real depth, assembled out of axis-aligned
slabs rather than drawn as a texture. The N64's logo spun a solid "N" in
three dimensions, and the thing that made it read on a CRT at 240 lines was
that it had thickness and caught the light differently on each face.

Two objects, exported separately so the splash can move them
independently:

  `kiln`    the Kiln wordmark — the piece that spins
  `demod`  a plate that sits behind it and does not

Everything is boxes. `kilnlib.box` splits corners per face, so each face
keeps its own flat normal and its own colour — which is exactly what a
faceted logo wants, and why the letters read as solid rather than as a soft
grey lump under the engine's single directional light.

── The glyphs ─────────────────────────────────────────────────────────────
KILN is four straight-stroke letters, which is why they are cheaper here
than the 6 and 4 this wordmark replaced: no glyph needs a curve
approximated by steps, only K and N need diagonals. Twelve slabs total.
Building letters from rectangles rather than from a font mesh keeps the
whole thing under 400 triangles and gives it the blocky, slightly
wrong-looking geometry a 1996 logo actually had.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kilnlib as m  # noqa: E402

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


def glyph_K(parts, x0, color):
    """K — an upright plus two arms climbing away from it in steps. The
    arms are slabs rather than true diagonals for the same reason the old
    M's were: a rotated quad needs its own normals and buys nothing at
    the size this is ever seen."""
    w = 0.72
    slab(parts, x0, H / 2, S, H, color)                          # upright
    slab(parts, x0 + w * 0.42, H * 0.66, S * 0.9, H * 0.26, color)
    slab(parts, x0 + w * 0.85, H * 0.86, S * 0.9, H * 0.28, color)
    slab(parts, x0 + w * 0.42, H * 0.34, S * 0.9, H * 0.26, color)
    slab(parts, x0 + w * 0.85, H * 0.14, S * 0.9, H * 0.28, color)


def glyph_I(parts, x0, color):
    """I — one upright, no serifs. At 240 lines a serif on a stroke this
    wide is a single pixel, and a single pixel reads as noise."""
    slab(parts, x0, H / 2, S, H, color)


def glyph_L(parts, x0, color):
    w = 0.56
    slab(parts, x0, H / 2, S, H, color)                          # upright
    slab(parts, x0 + w / 2, S / 2, w + S, S, color)              # foot


def glyph_N(parts, x0, color):
    """N — two uprights with a descending stair between them."""
    w = 0.72
    slab(parts, x0, H / 2, S, H, color)                          # left
    slab(parts, x0 + w, H / 2, S, H, color)                      # right
    slab(parts, x0 + w * 0.33, H * 0.72, S * 0.9, H * 0.34, color)
    slab(parts, x0 + w * 0.67, H * 0.38, S * 0.9, H * 0.34, color)


def build_wordmark():
    parts = {"verts": [], "faces": [], "colors": []}
    # One light letter then three accent ones, which is the rhythm the
    # old M-64 mark had. It does not encode anything; it just keeps the
    # same amount of red on screen, and the advances land the wordmark at
    # ~3.08 units so build_plate's 3.10 bar still sits under it.
    glyph_K(parts, 0.00, FACE)
    glyph_I(parts, 1.00, ACCENT)
    glyph_L(parts, 1.34, ACCENT)
    glyph_N(parts, 2.20, ACCENT)

    # Centre it on X so the splash can spin it about its own middle rather
    # than about its left edge — a logo that orbits instead of rotating is
    # the classic tell of a pivot left at the origin.
    xs = [v[0] for v in parts["verts"]]
    cx = (min(xs) + max(xs)) / 2.0
    parts["verts"] = [(v[0] - cx, v[1], v[2]) for v in parts["verts"]]

    m.make_material("kiln")
    m.make_mesh("kiln", parts["verts"], parts["faces"], "kiln",
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
