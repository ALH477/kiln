#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""gen_textures.py — the procedural texture set, written with the stdlib only.

Why not Pillow: this runs inside a Nix build, and a PNG encoder is ~60 lines of
zlib and struct. Adding a Python image stack to the closure to draw five 32x32
tiles is the wrong trade.

── The filename convention is load-bearing ────────────────────────────────
Textures are named `<name>.<fmt>.png`, e.g. `checker.i8.png`. Two independent
tools read that middle section and they have to agree:

  * mksprite tokenises the filename on '.' and picks the RDP surface format
    from whichever section names one (tools/mksprite/mksprite.c, strtok over
    tex_format_from_name), then writes `<basename-minus-last-ext>.sprite` —
    so `checker.i8.png` becomes `checker.i8.sprite`.

  * gltf_to_t3d bakes a runtime path into the .t3dm by replacing the FIRST
    ".png" in the path with ".sprite" (materialParser.cpp mapRomPath) — so it
    also produces `checker.i8.sprite`.

They line up only because the importer cuts at ".png" rather than at the last
dot. Rename a texture to `checker.png` and you lose format autodetection;
rename it to `checker.i8.v2.png` and the two names still agree but mksprite
sees an extra section. Keep to `<name>.<fmt>.png`.

── Sizes ─────────────────────────────────────────────────────────────────
TMEM is 4 KB total. At 32x32: I8 = 1 KB, IA8 = 1 KB, RGBA16 = 2 KB. All fit
with room for a second tile, which is the point of staying at 32x32.

Everything here is deterministic: no `random`, no dict iteration order, no
timestamps. nix/checks/assets.nix builds the outputs twice and compares.

Usage: gen_textures.py <outdir>
"""

import struct
import sys
import zlib
from pathlib import Path

SIZE = 32


# ── PNG ────────────────────────────────────────────────────────────────────
def write_png(path, width, height, pixels):
    """Write 8-bit RGBA (colour type 6). `pixels` is a flat bytes/bytearray of
    width*height*4.

    Always RGBA8 regardless of the target RDP format: mksprite decodes to RGBA
    and quantises to whatever the filename asked for, and gltf_to_t3d only
    decodes the PNG to learn its dimensions. Emitting one source format keeps
    this writer to a single code path.
    """
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter type 0 (None) — smaller code, tiles are tiny
        raw += pixels[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", ihdr)
           # level 9, fixed: deflate is deterministic for a given level+input.
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    Path(path).write_bytes(png)


# ── deterministic noise ────────────────────────────────────────────────────
def _hash2(x, y, seed):
    """Integer hash of a lattice point. Explicit arithmetic rather than
    random.Random so the output cannot drift with a CPython release."""
    h = (x * 374761393 + y * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    return (h ^ (h >> 16)) & 0xFFFFFFFF


def _smooth(t):
    return t * t * (3.0 - 2.0 * t)


def value_noise(x, y, period, seed):
    """Value noise on a lattice that wraps at `period`, so the tile is seamless
    when repeated — which is the whole reason to generate rather than paint."""
    x0, y0 = int(x) % period, int(y) % period
    x1, y1 = (x0 + 1) % period, (y0 + 1) % period
    fx, fy = _smooth(x - int(x)), _smooth(y - int(y))

    def v(a, b):
        return _hash2(a, b, seed) / 0xFFFFFFFF

    top = v(x0, y0) + (v(x1, y0) - v(x0, y0)) * fx
    bot = v(x0, y1) + (v(x1, y1) - v(x0, y1)) * fx
    return top + (bot - top) * fy


def fbm(x, y, size, seed, octaves=3):
    """Fractal sum of wrapping value noise. Each octave doubles the lattice
    frequency, and the lattice period doubles with it so every octave still
    wraps at the tile edge."""
    total, amp, norm, freq = 0.0, 1.0, 0.0, 4
    for o in range(octaves):
        total += value_noise(x * freq / size, y * freq / size, freq, seed + o) * amp
        norm += amp
        amp *= 0.5
        freq *= 2
    return total / norm


# ── the textures ───────────────────────────────────────────────────────────
def tex_checker(size):
    """8x8 checker. The one texture where a wrong result is unmistakable: if
    the UV pixel-coord conversion is off the squares stop being square, and if
    the tile params are wrong the repeat seam is visible."""
    px = bytearray()
    for y in range(size):
        for x in range(size):
            on = ((x // 8) + (y // 8)) % 2 == 0
            v = 0xE8 if on else 0x30
            px += bytes((v, v, v, 0xFF))
    return px


def tex_grid(size):
    """Coloured grid with an asymmetric corner mark. Reads UV orientation at a
    glance: a mirrored or transposed UV set looks fine on a checker and obvious
    here."""
    px = bytearray()
    for y in range(size):
        for x in range(size):
            edge = (x % 8 == 0) or (y % 8 == 0)
            if x < 4 and y < 4:
                r, g, b = 0xFF, 0x4C, 0x6A      # origin corner: red
            elif edge:
                r, g, b = 0x00, 0xF5, 0xD4      # grid lines: cyan
            else:
                shade = 0x28 + ((x // 8) + (y // 8)) * 0x14
                r, g, b = shade, shade, 0x50
            px += bytes((r, g, b, 0xFF))
    return px


def tex_grass(size):
    px = bytearray()
    for y in range(size):
        for x in range(size):
            n = fbm(x, y, size, seed=11, octaves=3)
            v = int(0x50 + n * 0x90)
            px += bytes((v, v, v, 0xFF))
    return px


def tex_rock(size):
    px = bytearray()
    for y in range(size):
        for x in range(size):
            n = fbm(x, y, size, seed=29, octaves=4)
            # Push toward the extremes so the grain survives quantisation to I8
            # and then to the RDP's 5-bit-ish effective contrast on screen.
            n = n * n * (3.0 - 2.0 * n)
            v = int(0x38 + n * 0xA0)
            px += bytes((v, v, v, 0xFF))
    return px


def tex_water(size):
    """IA8: intensity carries the highlight, alpha carries the transparency.
    The water quad is drawn with a MULTIPLY blender, so alpha here is what
    actually makes the terrain show through."""
    px = bytearray()
    for y in range(size):
        for x in range(size):
            n = fbm(x, y, size, seed=71, octaves=2)
            i = int(0x60 + n * 0x9F)
            a = int(0x60 + n * 0x50)
            px += bytes((i, i, i, a))
    return px


# The dark floor of the foam texture, as a fraction of full intensity.
#
# This number is a CONTRACT with whatever water mesh this texture is applied
# to, if that mesh is drawn with a multiply combiner (texel * vertex colour):
# the vertex colour becomes a CEILING, not the water's colour, so flat water
# is `vertex * FOAM_FLOOR` and a crest is `vertex * 1.0`. A generator meant
# to use this texture should author the sea's vertex colours at CREST
# brightness and let this texture carve the troughs back down. Raise the
# floor here and the whole sea gets lighter and flatter.
FOAM_FLOOR = 0x34


def tex_foam(size):
    """I8: moonlit crests on dark water, for the sea's scrolling overlay.

    Not tex_water: that one is a general-purpose transparent water sheet for
    a multiply blender over terrain. This is the sea SURFACE at night, where
    almost everything is dark trough and only the occasional crest catches
    the moon — so the field is thresholded hard rather than smoothly varying.
    A smooth noise here reads as fog on the water, not as swell.

    Plain wrapping fbm, thresholded hard. The crests are isotropic: two
    attempts at making them directional — scaling the sample coordinates,
    and averaging along a wrapped axis — were both dropped, the first
    because fbm only tiles when x and y each span exactly `size` (scaling a
    coordinate silently breaks the seam, measured as a 15/255 discontinuity
    across the tile edge) and the second because it attenuated the field
    without elongating it (anisotropy ratio moved 0.95 -> 1.07, i.e. not at
    all). Direction comes from the SCROLL instead, which is free and which
    the eye reads as flow anyway. Seamlessness is the property worth
    protecting here, because this is the one texture in this file that
    moves.

    KNEE is the 90th percentile of the field, so about a tenth of the tile
    is crest. That number is the look: at a quarter the sea is milk, at a
    fiftieth the moon path is bare.

    Measuring the seam: compare the wrap edge against the TYPICAL adjacent
    row/column difference of the raw field, not the finished bytes. The knee
    turns a 0.01 field difference into a 30-value jump wherever it happens to
    fall near the threshold, so the output edges can read as a seam when the
    field underneath is continuous — which it is here, measured at roughly
    half the typical adjacent variation on both axes.
    """
    KNEE = 0.697  # p90 of fbm(seed=311, octaves=3) over a 32x32 tile
    px = bytearray()
    for y in range(size):
        for x in range(size):
            n = fbm(x, y, size, seed=311, octaves=3)
            if n <= KNEE:
                # Troughs still vary a little, or flat water looks like a
                # painted plane once the swell tilts it toward the moon.
                i = FOAM_FLOOR + int((n / KNEE) * 0x14)
            else:
                t = min(1.0, (n - KNEE) / 0.09)
                i = FOAM_FLOOR + 0x14 + int(t * (0xFF - FOAM_FLOOR - 0x14))
            # RGBA, like every other generator here: write_png always emits
            # colour type 6 and the `.i8.` in the filename only tells mksprite
            # what to quantise DOWN to. Emitting one byte per pixel because
            # the target is I8 produces a PNG whose IDAT is a quarter the
            # length the header promises, which decodes as
            # "invalid decompressed idat size" three tools later.
            i = min(0xFF, i)
            px += bytes((i, i, i, 0xFF))
    return px


TEXTURES = [
    ("checker.i8.png", tex_checker),
    ("grid.rgba16.png", tex_grid),
    ("grass.i8.png", tex_grass),
    ("rock.i8.png", tex_rock),
    ("water.ia8.png", tex_water),
    ("foam.i8.png", tex_foam),
]


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2

    outdir = Path(argv[1])
    outdir.mkdir(parents=True, exist_ok=True)

    for name, fn in TEXTURES:
        path = outdir / name
        write_png(path, SIZE, SIZE, fn(SIZE))
        print(f"  [PNG] {name}  {SIZE}x{SIZE}  {path.stat().st_size} bytes")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
