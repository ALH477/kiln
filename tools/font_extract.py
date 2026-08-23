#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""font_extract.py — libdragon's builtin debug font, as a host C header.

    python3 tools/font_extract.py --builtin <rdpq_font_builtin.c> \
        --emit-header plat/host/include/kiln_host_font.h
    python3 tools/font_extract.py --builtin <rdpq_font_builtin.c> --verify

The host 2D layer has to lay text out exactly as the console does, or the one
thing it is for — checking that a HUD reads correctly — is worthless. So it
does not ship a lookalike font: it ships libdragon's own
FONT_BUILTIN_DEBUG_MONO, decoded out of the same 3,240-byte blob the ROM
links, into a plain glyph table a software rasteriser can consume.

── The .font64 format, as far as this needs it ────────────────────────────
Everything below was read off libdragon's src/rdpq/rdpq_font_internal.h and
confirmed against the blob's own bytes, not guessed. Big-endian throughout
(it is built for a MIPS target). "Pointers" in the file are byte offsets from
the start of the font — libdragon's loader turns them into pointers with
PTR_DECODE, and so does this.

    off  0  magic "FNT" + version (must be 11)
         4  flags; low nibble is the fonttype_t
         8  point_size
        12  ascent, descent, line_gap, space_width  (int32 each)
        28  ellipsis_{width,glyph,reps,advance}     (int16/uint16)
        36  num_{ranges,glyphs,atlases,kerning,styles}
        56  builtin_style: color, outline_color, custom fn, custom arg
        72  offsets: ranges, sparse_range, glyphs, glyph_kranges,
            atlases, kerning, styles

The builtin font is the easy case and this asserts every part of that rather
than assuming it: ONE dense range (0x20..0xBF, first_glyph 0) so there is no
CHD perfect-hash table to walk, num_kerning == 0 so advances are the whole of
layout, and ONE atlas.

── The glyph records ──────────────────────────────────────────────────────
Eight bytes each: xadvance, xoff, yoff, xoff2, yoff2, s, t, then a packed
byte of natlas:6 / ntile:2. The box is (xoff2-xoff) x (yoff2-yoff) and (s,t)
is where it sits in the atlas.

── The atlas, which is the part worth writing down ────────────────────────
The atlas is a libdragon sprite: uint16 width, uint16 height, then bitdepth,
flags, hslices, vslices, then pixels. flags & 0x1F is the RDP texture format,
and for this font it is 0x08 = CI4.

FONT_TYPE_MONO_OUTLINE is documented as "CI4, which are 2 2bpp layers", and
that is literally true: each 4-bit index holds TWO glyphs' 2-bit coverage,
and a glyph's `ntile` says which half is his —

    coverage = (index >> (2 * ntile)) & 3

with 0 transparent, 1 the fill and 2 the outline. 39 of the 83 atlas slots
are shared by two glyphs this way, which is how a 160-glyph font fits in a
96x34 image. Decoding only one half, or picking the wrong half, produces
glyphs that are recognisable but subtly wrong — so --verify prints the whole
table for a human to look at, and every glyph box is bounds-checked against
the atlas below, because reading past a row silently yields a plausible
letter made of its neighbours.
"""
import argparse
import re
import struct
import sys

FIRST_CP = 0x20
LAST_CP = 0xBF


class Font:
    def __init__(self, blob):
        self.b = blob
        u32 = self.u32
        i32 = self.i32
        if blob[:3] != b"FNT":
            raise SystemExit(f"font_extract: bad magic {blob[:3]!r}, expected b'FNT'")
        self.version = blob[3]
        if self.version != 11:
            raise SystemExit(
                f"font_extract: font64 version {self.version}, this tool speaks 11. "
                "libdragon changed the format; re-read rdpq_font_internal.h."
            )
        self.flags = u32(4)
        self.fonttype = self.flags & 0x0F
        if self.fonttype != 2:
            raise SystemExit(
                f"font_extract: fonttype {self.fonttype}, expected 2 "
                "(FONT_TYPE_MONO_OUTLINE). The 2x2bpp layer decode below is "
                "specific to it."
            )
        self.point_size = u32(8)
        self.ascent, self.descent = i32(12), i32(16)
        self.line_gap, self.space_width = i32(20), i32(24)
        self.num_ranges, self.num_glyphs = u32(36), u32(40)
        self.num_atlases, self.num_kerning = u32(44), u32(48)

        o_ranges, o_sparse, o_glyphs = u32(72), u32(76), u32(80)
        o_atlases = u32(88)

        if self.num_ranges != 1:
            raise SystemExit(f"font_extract: {self.num_ranges} ranges, expected 1")
        if o_sparse != 0:
            raise SystemExit(
                "font_extract: font has a sparse range table (CHD perfect hash). "
                "The builtin font's range is dense; a sparse one needs the hash "
                "walk this tool deliberately does not implement."
            )
        if self.num_kerning != 0:
            raise SystemExit(
                f"font_extract: {self.num_kerning} kerning pairs. Layout here "
                "assumes advances are the whole story."
            )
        if self.num_atlases != 1:
            raise SystemExit(f"font_extract: {self.num_atlases} atlases, expected 1")

        first_cp, n_cp, first_glyph = u32(o_ranges), u32(o_ranges + 4), i32(o_ranges + 8)
        if (first_cp, first_glyph) != (FIRST_CP, 0) or n_cp != self.num_glyphs:
            raise SystemExit(
                f"font_extract: unexpected range {first_cp:#x}+{n_cp} "
                f"first_glyph={first_glyph}"
            )
        self.first_cp, self.n_cp = first_cp, n_cp

        # ── the atlas ──
        sp = u32(o_atlases)
        self.aw, self.ah = struct.unpack_from(">HH", blob, sp)
        sflags = blob[sp + 5]
        fmt = sflags & 0x1F
        if fmt != 0x08:
            raise SystemExit(
                f"font_extract: atlas texture format {fmt:#x}, expected 0x08 (CI4)"
            )
        self.adata = sp + 8
        self.astride = self.aw // 2

        # ── the glyphs ──
        self.glyphs = {}
        for i in range(self.num_glyphs):
            adv, xo, yo, xo2, yo2, s, t, nt = struct.unpack_from(
                ">BbbbbBBB", blob, o_glyphs + 8 * i
            )
            cp = first_cp + i
            w, h = xo2 - xo, yo2 - yo
            if adv == 0 and w == 0:
                continue  # codepoint present in the range but with no glyph
            if w == 0 and h == 0:
                # Space: a real advance and nothing to draw. Recorded so
                # layout steps over it, with no entry in the coverage blob.
                self.glyphs[cp] = dict(adv=adv, xo=0, yo=0, w=0, h=0,
                                       s=0, t=0, ntile=nt & 3)
                continue
            if w <= 0 or h <= 0:
                raise SystemExit(f"font_extract: cp {cp:#x} has box {w}x{h}")
            # Bounds, loudly. A glyph read past a row's end comes back as a
            # plausible letter built out of its neighbours' pixels.
            if s + w > self.aw or t + h > self.ah:
                raise SystemExit(
                    f"font_extract: cp {cp:#x} box {w}x{h} at ({s},{t}) runs off "
                    f"the {self.aw}x{self.ah} atlas — the record layout is being "
                    "misread."
                )
            self.glyphs[cp] = dict(
                adv=adv, xo=xo, yo=yo, w=w, h=h, s=s, t=t, ntile=nt & 3
            )

    def u32(self, o):
        return struct.unpack_from(">I", self.b, o)[0]

    def i32(self, o):
        return struct.unpack_from(">i", self.b, o)[0]

    def index(self, x, y):
        byte = self.b[self.adata + y * self.astride + x // 2]
        return (byte >> 4) if (x % 2 == 0) else (byte & 0x0F)

    def coverage(self, g, x, y):
        """0 transparent, 1 fill, 2 outline.

        `ntile` 0 takes the HIGH two bits of the index and 1 takes the low
        two — the inverse of the obvious reading, and settled by rendering
        letters under both mappings rather than by reasoning about it. Under
        `2 * ntile` every glyph comes out as recognisable garbage built from
        its slot-mate's pixels; under this one, A/E/5 (ntile 0) and T
        (ntile 1) all read correctly. It mirrors CI4's own high-nibble-first
        pixel order. --verify prints the whole table so the next person can
        check rather than trust this comment.
        """
        shift = 2 * (1 - g["ntile"])
        return (self.index(g["s"] + x, g["t"] + y) >> shift) & 3


def read_builtin(path):
    """Pull the __fontdb_monogram byte array out of libdragon's generated C.

    rdpq_font_builtin.c holds two fonts (monogram and at01, in that order);
    FONT_BUILTIN_DEBUG_MONO is the first. Matching the name rather than
    taking the first array keeps that from silently becoming the wrong one.
    """
    txt = open(path).read()
    m = re.search(
        r"unsigned char __fontdb_monogram\[\]\s*=\s*\{(.*?)\};", txt, re.S
    )
    if not m:
        raise SystemExit(
            f"font_extract: no __fontdb_monogram array in {path}. libdragon may "
            "have renamed the builtin font; check src/rdpq/mkfontbuiltin.sh."
        )
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))


def emit_header(f, out):
    lines = []
    A = lines.append
    A("/* SPDX-License-Identifier: MIT")
    A(" *")
    A(" * kiln_host_font.h — GENERATED by tools/font_extract.py. Do not edit.")
    A(" *")
    A(" * libdragon's FONT_BUILTIN_DEBUG_MONO, decoded out of the same blob the")
    A(" * ROM links, so host text lays out exactly as console text does. See the")
    A(" * generator for the .font64 format and for why the 2bpp layer decode is")
    A(" * the part worth being careful about.")
    A(" *")
    A(" * nix/checks/kiln-font.nix regenerates this and diffs, so a libdragon")
    A(" * bump that changes the font cannot drift past unnoticed.")
    A(" */")
    A("#ifndef KILN_HOST_FONT_H")
    A("#define KILN_HOST_FONT_H")
    A("")
    A("#include <stdint.h>")
    A("")
    A(f"#define KILN_FONT_ASCENT      {f.ascent}")
    A(f"#define KILN_FONT_DESCENT     {f.descent}")
    A(f"#define KILN_FONT_LINE_GAP    {f.line_gap}")
    A(f"#define KILN_FONT_SPACE_WIDTH {f.space_width}")
    A(f"#define KILN_FONT_FIRST_CP    {f.first_cp:#04x}")
    A(f"#define KILN_FONT_LAST_CP     {f.first_cp + f.n_cp - 1:#04x}")
    A("")
    A("/* One byte per pixel of coverage: 0 transparent, 1 fill, 2 outline. Row")
    A(" * major, `w` bytes per row, `h` rows. Kept a byte per pixel rather than")
    A(" * packed 2bpp because the whole table is under 12 KB and a host build")
    A(" * has no TMEM to answer to — the console's constraint does not apply to")
    A(" * the machine describing it. */")
    A("typedef struct {")
    A("    uint8_t  advance;   /* pixels to step the cursor                  */")
    A("    int8_t   xoff, yoff;/* box origin, relative to cursor + baseline  */")
    A("    uint8_t  w, h;      /* box size; 0x0 means no glyph               */")
    A("    uint16_t bits;      /* offset into kiln_font_bits, or 0           */")
    A("} KilnFontGlyph;")
    A("")

    blobs = []
    off = 0
    recs = []
    for cp in range(f.first_cp, f.first_cp + f.n_cp):
        g = f.glyphs.get(cp)
        if g is None:
            recs.append((cp, 0, 0, 0, 0, 0, 0))
            continue
        if g["w"] == 0:
            recs.append((cp, g["adv"], 0, 0, 0, 0, 0))
            continue
        cov = [f.coverage(g, x, y) for y in range(g["h"]) for x in range(g["w"])]
        blobs.append((cp, cov))
        recs.append((cp, g["adv"], g["xo"], g["yo"], g["w"], g["h"], off))
        off += len(cov)

    A(f"static const uint8_t kiln_font_bits[{off}] = {{")
    for cp, cov in blobs:
        ch = chr(cp) if 0x20 < cp < 0x7F else "?"
        A(f"    /* {cp:#04x} {ch!r} */")
        for i in range(0, len(cov), 24):
            A("    " + "".join(f"{v}," for v in cov[i : i + 24]))
    A("};")
    A("")
    A(f"static const KilnFontGlyph kiln_font_glyphs[{f.n_cp}] = {{")
    for cp, adv, xo, yo, w, h, boff in recs:
        ch = chr(cp) if 0x20 < cp < 0x7F else " "
        A(
            f"    {{ {adv:3d}, {xo:3d}, {yo:3d}, {w:2d}, {h:2d}, {boff:5d} }},"
            f" /* {cp:#04x} {ch} */"
        )
    A("};")
    A("")
    A("#endif /* KILN_HOST_FONT_H */")

    text = "\n".join(lines) + "\n"
    if out == "-":
        sys.stdout.write(text)
    else:
        open(out, "w").write(text)
        print(f"font_extract: wrote {out} "
              f"({len(f.glyphs)} glyphs, {off} coverage bytes)")


def verify(f):
    print(
        f"font64 v{f.version} type {f.fonttype} (MONO_OUTLINE)  atlas {f.aw}x{f.ah} CI4"
    )
    print(
        f"ascent {f.ascent} descent {f.descent} line_gap {f.line_gap} "
        f"space_width {f.space_width}"
    )
    print(f"{len(f.glyphs)} glyphs of {f.n_cp} codepoints; "
          f"{f.n_cp - len(f.glyphs)} codepoints have none")
    advs = sorted({g["adv"] for g in f.glyphs.values()})
    print(f"advances present: {advs}  <- one value means the font is monospaced")
    missing = [cp for cp in range(0x20, 0x7F) if cp not in f.glyphs]
    print(f"printable ASCII 0x20-0x7E without a glyph: {missing or 'none'}")
    print()
    for cp in range(0x21, 0x7F):
        g = f.glyphs.get(cp)
        if not g:
            continue
        rows = [
            "".join(" .:#"[f.coverage(g, x, y)] for x in range(g["w"]))
            for y in range(g["h"])
        ]
        print(f"  {cp:#04x} {chr(cp)!r} adv={g['adv']} box={g['w']}x{g['h']} "
              f"ntile={g['ntile']}")
        for r in rows:
            print("        |" + r + "|")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--builtin", required=True,
                    help="path to libdragon's src/rdpq/rdpq_font_builtin.c")
    ap.add_argument("--emit-header")
    ap.add_argument("--verify", action="store_true")
    a = ap.parse_args()
    f = Font(read_builtin(a.builtin))
    if a.verify:
        verify(f)
    if a.emit_header:
        emit_header(f, a.emit_header)
    if not a.verify and not a.emit_header:
        ap.error("nothing to do: pass --verify and/or --emit-header")


main()
