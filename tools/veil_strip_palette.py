#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""veil_strip_palette.py — detach a CI4 sprite's embedded palette.

    python3 tools/veil_strip_palette.py <file.sprite>

Companion to tools/veil_palette.py, which bakes the cold/veiled pair a veil
material's TLUT ramp is built from. That palette is the one the game binds at
runtime; this script removes the *other* one — the copy mksprite embeds in the
sprite itself — because on this hardware the two collide and the embedded one
wins.

── The collision, in the order it happens ─────────────────────────────────
Tiny3D's set_texture() applies one material's texture:

    t3dmodel.c:147    conf->tileCb(...)          the tile callback
    t3dmodel.c:158    rdpq_sprite_upload(...)    eleven lines later

The tile callback is the only hook a caller has into tile setup, and it is
where a per-material TLUT has to go: a palette bound before
t3d_model_draw_material is overwritten by the material, and there is no
callback after the upload. So a veil material binds its ramp at :147.

Then :158 runs. rdpq_sprite_upload calls sprite_upload_palette
(rdpq_sprite.c:17-35), which uploads the sprite's own palette and sets
rdpq_mode_tlut. Both writes target the same 16-entry TMEM block — the veil
binds at `tmem_tile * 16` with tmem_tile 0 for every material, and
sprite_upload_palette writes at `palidx * 16` with palidx 0 — so the ramp is
written and immediately overwritten, every frame, on every material.

The symptom is not a crash or a missing texture. The texture arrives, in the
palette it was authored with, which looks like a correct picture.

── What this does and does not fix ────────────────────────────────────────
The clobber above is real and provable without running anything: mksprite
writes a 512-byte palette into the sprite, sprite_ext_t.pal_file_pos points at
it, and rdpq_sprite.c uploads it to the block the veil just wrote. Removing it
is correct on its own terms.

It is also, as of this writing, NOT SUFFICIENT to make the palette swap
visible. PetaByte Madness' `.#pm-veil-ab-diag` versus `.#pm-veil-ab-real` —
two ROMs differing only in the bytes of three .pal files, both seeked to the
same frame with the veil forced on and the textured model on screen — still
diff to zero pixels. So a second cause remains somewhere between the ramp the
game has loaded and the TLUT the RDP looks through. This step stays because it
removes a defect that would otherwise have to be found again later, behind
whatever the remaining one turns out to be.

── Why this is the right layer to fix it ──────────────────────────────────
rdpq_sprite.c's own comment, three lines above the clobber, names this case:

    "We account for sprites being CI4 but without embedded palette: mksprite
     doesn't create sprites like this today, but it could in the future
     (eg: sharing a palette across [sprites])."

A veil material is exactly that — its palette lives in the .pal sidecar,
loaded once at boot and expanded into a nine-step ramp. With
sprite_ext_t.pal_file_pos zeroed, sprite_get_palette returns NULL
(sprite.c:221-226), sprite_upload_palette still sets rdpq_mode_tlut correctly
and skips the upload, and whatever the tile callback bound survives.

Patching the file rather than libdragon or Tiny3D is deliberate: this changes
nothing for any other sprite in any other ROM, and a patch to either library
would. It is also why this refuses rather than shrugs when its assumptions
stop holding — a strip step that silently does nothing is invisible until
someone counts pixels again.
"""
import struct
import sys

# sprite_t flags (libdragon include/sprite.h:87-90).
SPRITE_FLAGS_TEXFORMAT = 0x1F
SPRITE_FLAGS_NODATA = 0x40
SPRITE_FLAGS_EXT = 0x80

FMT_CI4 = 0x08  # tex_format_t


def strip(path):
    with open(path, "rb") as f:
        b = bytearray(f.read())

    # sprite_t: width u16, height u16, bitdepth u8, flags u8, hslices u8,
    # vslices u8 — 8 bytes, big-endian, because the file is built for MIPS.
    width, height, _bitdepth, flags = struct.unpack_from(">HHBB", b, 0)
    fmt = flags & SPRITE_FLAGS_TEXFORMAT

    if not flags & SPRITE_FLAGS_EXT:
        sys.exit("%s: no extended header. There is nothing to strip — and no "
                 "palette for the RDP to find either, so this sprite cannot "
                 "be a veil material." % path)

    # Asserted rather than assumed: the veil is CI4 + TLUT by design
    # (docs/VEIL_DESIGN.md section 1), and a sprite that came out as anything
    # else has already lost the mechanic before this step runs.
    if fmt != FMT_CI4:
        sys.exit("%s: format 0x%02X, expected CI4 (0x%02X). A veil material "
                 "must be CI4 — the palette swap has nothing to swap "
                 "otherwise." % (path, fmt, FMT_CI4))

    # With NODATA the ext header sits immediately after the 8-byte base
    # header; otherwise it follows the embedded pixel data (sprite.c:28-29).
    if flags & SPRITE_FLAGS_NODATA:
        ext = 8
    else:
        ext = 8 + (width * height + 1) // 2  # CI4 packs two pixels per byte

    # sprite_ext_t: size u16, version u16, pal_file_pos u32.
    size, version, pal_pos = struct.unpack_from(">HHI", b, ext)
    if size == 0 or ext + size > len(b):
        sys.exit("%s: ext header at offset %d is implausible (size %d, file "
                 "is %d bytes). The pixel-data stride assumed above is "
                 "probably wrong for this sprite." % (path, ext, size, len(b)))

    # A zero here already means mksprite stopped embedding palettes, at which
    # point this script is dead code pretending to do something. Fail loudly:
    # the entire point is that its absence is invisible until someone counts
    # pixels in a capture.
    if pal_pos == 0:
        sys.exit("%s: pal_file_pos is already 0. mksprite no longer embeds a "
                 "palette, so this step is obsolete — remove it from "
                 "mkVeilTexture and re-verify that the veil still swaps."
                 % path)

    struct.pack_into(">I", b, ext + 4, 0)
    with open(path, "wb") as f:
        f.write(bytes(b))

    print("  veil: %s %dx%d CI4, ext v%d at %d — palette at %d detached"
          % (path.rsplit("/", 1)[-1], width, height, version, ext, pal_pos))


def main(argv):
    if len(argv) != 2:
        sys.exit("usage: veil_strip_palette.py <file.sprite>")
    strip(argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
