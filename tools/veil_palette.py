#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""veil_palette.py — build the scarlet veil's cold/veiled TLUT pairs.

    python3 tools/veil_palette.py <in.png> <out.pal> --class demon
    python3 tools/veil_palette.py --selftest

The palette-swap trick this bakes for (see the n64-modeling skill's "CI4 and
a palette-swap contract"): author both states offline, and at runtime change
only which 16-entry lookup table a material points at. 32 bytes of DMA, zero
extra pixels shaded, against a blended full-screen quad's worth of
read-modify-writes for the same effect done as a screen filter.

This file is that bake's palette half; `nix/assets.nix`'s `mkVeilTexture` is
the pixel half, and `kiln_voxmesh`'s `kiln_voxatlas_bind` is the generic
engine-side entry point that swaps which TLUT of a pair is resident. A
specific game's own runtime binds a palette per-material and per-frame on
top of that; this tool only has to agree on the file format and the
class-to-palette contract below.

── What comes out ─────────────────────────────────────────────────────────
A `.pal` file: 32 big-endian uint16 in RGBA5551, cold palette first, then
veiled — a pair a runtime can also crossfade between over several frames
rather than swapping in one step, if it wants a transition rather than a cut.

Big-endian because the N64 is, and because the runtime hands the bytes straight
to rdpq_tex_upload_tlut without swapping.

── The four material classes ───────────────────────────────────────────────
Under the veil hue carries no information and only VALUE does, so value is
rationed:

  world    Environment. Collapses to a BAND — it may not own true black or
           true white, because those are reserved. Under night-vision gain
           that band is bright and tight; it is a ration, not a dimming.
  demon    Full range: owns both ends. That contrast is what physiologically
           drags the player's eye onto the creature.
  phantom  A body meant to be invisible until the veil is up. Its VEILED
           palette is the demon one; its COLD palette has alpha 0 on EVERY
           entry, which under an alpha-compare render mode means no pixel of
           it is written. A runtime can go further and skip submitting the
           display list at all while the veil is down, so an invisible body
           costs nothing rather than merely rendering as nothing.
  eyes     The exception that makes the trick frightening rather than merely
           cheap: the cold palette keeps alpha on its BRIGHTEST few entries, so
           veil down you get pinpricks in the dark and nothing else.

── Deliberately no libm-style cleverness, and no colour science ────────────
This runs on the host at build time, so it could afford CIELAB. It does not use
it: the target is a 5-bit-per-channel palette on a console whose output most
players will see through composite video, and the design's requirement is
ordinal (demons own the extremes, world does not) rather than perceptual. Rec.
601 luma is enough to order sixteen colours by value, and being able to state
what the transform does in one line is worth more here than being right in the
third decimal.

── Determinism ────────────────────────────────────────────────────────────
No dict-iteration order, no randomness, no wall clock: nix/checks/assets.nix
builds every asset twice and diffs the bytes, and mkVeilTexture is on that list.
"""

import argparse
import struct
import sys

CLASSES = ("world", "demon", "phantom", "eyes")

# The mid band `world` collapses into, as a fraction of full range. Demons own
# everything outside it. 0.28..0.72 leaves each end genuinely theirs while still
# giving the environment enough range to read as geometry rather than as fog.
WORLD_LO, WORLD_HI = 0.45, 0.88

# ── The veil is red NIGHT VISION ───────────────────────────────────────────
# Not a red tint over the picture: an intensifier. The fiction is that Horner
# can see what is actually down there in the dark, so the veiled state has to
# read as amplified darkness — bright, monochrome, grainy-looking because the
# value range is stretched — and not as the same room with a filter on it.
#
# That is a gamma well below 1 and a gain above it. The gamma lifts the darks
# (a night-vision tube's whole job) and the gain drives the midtones to the
# top of the range; together, a source value of 0.5 comes back at 0.99. Both
# still pin zero, so `demon` keeps true black.
VEIL_LIFT = 0.45
VEIL_GAIN = 1.35

# Above this, the tube blooms toward white the way a real intensifier does
# when it saturates. Green and blue come up together so the brightest entries
# read as white-hot rather than merely as more red — but capped well under
# red, because "every entry the veil renders is red-dominant" is asserted
# below and losing it would mean hue had crept back in at the top.
BLOOM_KNEE = 0.80
BLOOM_G, BLOOM_B = 0.55, 0.45

# How many of the brightest entries an `eyes` material keeps visible with the
# veil DOWN — few enough to read as pinpricks in the dark, not a lit face.
EYES_KEEP = 4


def luma(rgb):
    """Rec. 601 luma, 0..1. How BRIGHT an entry looks with the veil DOWN.

    Used only to rank entries for the `eyes` class, where the question really
    is perceptual: which few colours of the authored art still read as lit
    pinpricks to someone looking at the cold palette. It is deliberately NOT
    what drives the veiled transform — see value() for why.
    """
    r, g, b = rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0
    return 0.299 * r + 0.587 * g + 0.114 * b


def value(rgb):
    """HSV value — the largest channel, 0..1. What drives the VEILED palette.

    This used to be luma, and that was wrong in a way that took a capture to
    see: Rec. 601 weights red at 0.299, so a red source has LOW luma, and
    driving the veiled red channel from it made every red thing in the art
    come out darker under the veil than it went in. Blood and flesh — the
    things the scarlet veil exists to show you — dimmed when it was raised.
    The docstring already said this must not happen ("the veil reads as
    seeing MORE rather than as a dimmer"); the arithmetic said otherwise.

    Value has no such bias: a saturated red, green and blue all have value
    1.0 and all land in the same place. That is also what section 4 asks for
    in the first place — "under the veil hue carries no information" — which
    luma cannot deliver, because ordering by luma IS ordering by hue for
    equally-bright colours.
    """
    return max(rgb[0], rgb[1], rgb[2]) / 255.0


def to_rgba5551(rgb, opaque):
    """(r, g, b) 0-255 -> RGBA5551.

    Layout is rrrrrggg ggbbbbba — five bits each and the alpha in bit 0, the
    layout `rdpq_tex_upload_tlut` expects. The shift by 3 is a truncation,
    not a round: rounding can carry 255 to 32 and overflow the field into
    the next channel.
    """
    r = (min(255, max(0, int(rgb[0]))) >> 3) & 0x1F
    g = (min(255, max(0, int(rgb[1]))) >> 3) & 0x1F
    b = (min(255, max(0, int(rgb[2]))) >> 3) & 0x1F
    return (r << 11) | (g << 6) | (b << 1) | (1 if opaque else 0)


def veiled_rgb(rgb, cls):
    """The colour this entry becomes at full veil.

    Hue collapses toward red and value is redistributed per the material's
    class. Red is kept at (or above) the original luma so the veil reads as
    seeing MORE rather than as a dimmer; green and blue are crushed hard, which
    is what collapses hue.
    """
    # Intensify FIRST, ration second. The other order cannot work: a band that
    # tops out at WORLD_HI and is then gained by 1.35 clips straight through
    # its own ceiling, and `world` reaches the true white the whole class
    # contract reserves for creatures.
    v = min(1.0, (value(rgb) ** VEIL_LIFT) * VEIL_GAIN)

    if cls == "world":
        # The ration, and it is much TIGHTER than it was — 0.45..0.88 rather
        # than 0.28..0.72 — because everything is brighter now and the band
        # has to stay clear of both ends anyway. The environment cannot reach
        # true black or true white, so anything that does is a creature, and
        # that is what drags the eye onto it.
        v = WORLD_LO + v * (WORLD_HI - WORLD_LO)
    # else: demons and their eyes get the full range, unrationed. No
    # expansion about the midpoint any more — that crushed the bottom of the
    # range to black, which is right for a stylised filter and wrong for an
    # intensifier, whose entire purpose is that dark things become visible.

    # Monochrome red, with a white-hot bloom at the top. The 255:30:24 base
    # ratio is what collapses hue; the bloom is a VALUE cue, not a hue one —
    # it says "this is saturating the tube", and it says it identically
    # whatever colour the source was.
    bloom = 0.0
    if v > BLOOM_KNEE:
        bloom = (v - BLOOM_KNEE) / (1.0 - BLOOM_KNEE)

    r = v * 255.0
    g = v * (30.0 + bloom * BLOOM_G * 255.0)
    b = v * (24.0 + bloom * BLOOM_B * 255.0)
    return (r, g, b)


def build_pair(palette, cls):
    """palette: [(r,g,b), ...] up to 16 entries. -> (cold[16], veiled[16]).

    Both lists are always 16 long: TMEM's TLUT slot is 16 entries whatever the
    image uses, and a short upload would leave whatever the previous material
    put there in the unused entries.
    """
    if len(palette) > 16:
        raise SystemExit("veil_palette: %d colours, CI4 allows 16. Quantise "
                         "the source first (mksprite --format CI4 will, but "
                         "then its palette and this one would be two "
                         "different quantisations)." % len(palette))

    entries = list(palette) + [(0, 0, 0)] * (16 - len(palette))

    # Rank by value, so "the brightest four" is well defined for `eyes`
    # regardless of what order the PNG's palette happens to be in. Ties break
    # on index, which keeps it deterministic.
    order = sorted(range(16), key=lambda i: (luma(entries[i]), i), reverse=True)
    brightest = set(order[:EYES_KEEP])

    cold, veiled = [], []
    for i, rgb in enumerate(entries):
        if cls == "phantom":
            # Every entry transparent: the creature is not there at all.
            cold.append(to_rgba5551(rgb, opaque=False))
        elif cls == "eyes":
            cold.append(to_rgba5551(rgb, opaque=(i in brightest)))
        else:
            cold.append(to_rgba5551(rgb, opaque=True))

        # The veiled state of a phantom is a demon — the class only changes
        # what the COLD state is.
        veiled.append(to_rgba5551(
            veiled_rgb(rgb, "demon" if cls in ("phantom", "eyes") else cls),
            opaque=True))

    return cold, veiled


def pack(cold, veiled):
    """32 big-endian uint16: cold then veiled."""
    return struct.pack(">32H", *(list(cold) + list(veiled)))


def read_palette(path):
    """The 16-entry palette of an indexed PNG, as [(r,g,b), ...]."""
    from PIL import Image
    im = Image.open(path)
    if im.mode != "P":
        raise SystemExit(
            "veil_palette: %s is mode %s, not P (indexed).\n"
            "  The veil needs the SOURCE's own palette, because the runtime\n"
            "  swaps TLUTs against texel indices that were quantised once.\n"
            "  Quantise it first, e.g. in the generator that produced it."
            % (path, im.mode))
    raw = im.getpalette() or []
    n = len(im.getcolors() or []) or (len(raw) // 3)
    return [tuple(raw[i * 3:i * 3 + 3]) for i in range(min(n, 16))]


# ── Self-test ──────────────────────────────────────────────────────────────
# The properties below are the design contract, and every one of them is
# invisible in a screenshot: a phantom whose cold alpha is set draws a creature
# that should not be there, and a world palette that reaches true white steals
# the contrast the demons are supposed to own. Neither fails a build.
def selftest():
    fails = []

    def check(cond, msg):
        print(("  ok   " if cond else "  FAIL ") + msg)
        if not cond:
            fails.append(msg)

    RAMP = [(i * 17, i * 17, i * 17) for i in range(16)]   # black -> white

    def alpha(x):
        return x & 1

    def red5(x):
        return (x >> 11) & 0x1F

    print("── phantom ──")
    cold, veiled = build_pair(RAMP, "phantom")
    check(all(alpha(c) == 0 for c in cold),
          "cold: EVERY entry is alpha 0 (the creature is not drawn)")
    check(all(alpha(v) == 1 for v in veiled),
          "veiled: every entry is opaque")

    print("── eyes ──")
    cold, veiled = build_pair(RAMP, "eyes")
    lit = sum(alpha(c) for c in cold)
    check(lit == EYES_KEEP,
          "cold: exactly %d entries survive (%d)" % (EYES_KEEP, lit))
    # And they must be the BRIGHTEST ones — pinpricks, not a dim smear.
    lit_reds = [red5(c) for c in cold if alpha(c)]
    dark_reds = [red5(c) for c in cold if not alpha(c)]
    check(lit_reds and min(lit_reds) >= max(dark_reds),
          "cold: the survivors are the brightest entries")

    print("── world vs demon: the value ration ──")
    _, w = build_pair(RAMP, "world")
    _, d = build_pair(RAMP, "demon")
    wr = [red5(x) for x in w]
    dr = [red5(x) for x in d]
    check(max(wr) < 31, "world never reaches true white (max red %d/31)" % max(wr))
    check(min(wr) > 0, "world never reaches true black (min red %d/31)" % min(wr))
    check(max(dr) >= 31, "demons DO own true white (max red %d/31)" % max(dr))
    check(min(dr) == 0, "demons DO own true black (min red %d/31)" % min(dr))
    check(max(dr) > max(wr) and min(dr) < min(wr),
          "demons own both ends of the range the world cannot reach")

    print("── hue collapses ──")
    # A saturated blue and a saturated red must come out at THE SAME place:
    # under the veil, hue carries no information (§4). If they did not, a
    # palette distinguished only by hue would still read and the whole
    # value-separation discipline would be unenforced.
    #
    # This assertion used to be `red5(red) > red5(blue)` — "value still orders
    # them (red is brighter than blue in luma)" — which contradicted the
    # comment directly above it and was the visible end of the luma bug. Two
    # equally-saturated colours differing only in hue came out at different
    # brightnesses, which is exactly the leak this section exists to close.
    _, blue = build_pair([(0, 0, 255)], "world")
    _, red = build_pair([(255, 0, 0)], "world")
    _, green = build_pair([(0, 255, 0)], "world")
    check(red5(red[0]) == red5(blue[0]) == red5(green[0]),
          "three saturated hues land on ONE red (%d/%d/%d) — hue is gone"
          % (red5(red[0]), red5(green[0]), red5(blue[0])))

    # Below the bloom knee the picture is red and nothing else. This used to
    # be asserted on a SATURATED blue, which no longer works and should not:
    # under night-vision gain a saturated anything drives the tube to
    # saturation, and a saturating tube blooms white. So the sample is a dim
    # one, where the intensifier is not clipping and the only channel doing
    # any work is red.
    _, dim = build_pair([(0, 0, 60)], "world")
    check((dim[0] >> 6 & 0x1F) <= 4 and (dim[0] >> 1 & 0x1F) <= 4,
          "below the bloom knee, green and blue are ~0 (%d, %d)"
          % (dim[0] >> 6 & 0x1F, dim[0] >> 1 & 0x1F))

    # And the bloom itself: the brightest entries must actually go white-hot,
    # or the "intensifier saturating" reading is just a claim in a comment.
    # Still red-dominant — that is asserted separately below — but visibly
    # lifted off zero, which is what separates a bloom from a tint.
    _, hot = build_pair([(255, 255, 255)], "demon")
    check((hot[0] >> 6 & 0x1F) >= 8 and (hot[0] >> 1 & 0x1F) >= 6,
          "a saturating entry blooms toward white (g %d, b %d of 31)"
          % (hot[0] >> 6 & 0x1F, hot[0] >> 1 & 0x1F))

    print("── the veil is not a dimmer ──")
    # The whole point of the filter is that raising it shows you MORE. Every
    # entry the veil actually renders must therefore come back at least as
    # red as it went in — otherwise blood and flesh, the things it exists to
    # reveal, go DARKER when it comes up. Red art has low luma, so the old
    # luma-driven transform failed this badly and nothing caught it: a
    # capture showed FEWER red-leaning pixels with the veil raised than with
    # it down.
    #
    # Checked from the midpoint up. Below it the `demon` expansion
    # deliberately crushes the bottom of the range to true black, which is
    # the other half of the class contract and is not a dimming bug.
    SWATCHES = [(255, 0, 0), (200, 30, 30), (128, 0, 0), (160, 90, 60),
                (255, 255, 255), (180, 180, 180), (0, 255, 0), (0, 0, 255)]
    dim = []
    for rgb in SWATCHES:
        cold_r = (rgb[0] >> 3) & 0x1F
        c, v = build_pair([rgb], "demon")
        if value(rgb) >= 0.5 and red5(v[0]) < cold_r:
            dim.append((rgb, cold_r, red5(v[0])))
    check(not dim,
          "no swatch at or above mid value loses red under the veil" +
          ("" if not dim else " — %r" % (dim,)))

    # And what it renders must be unmistakably RED, not merely brighter.
    # Strict dominance, not a margin. At 5 bits the green and blue channels
    # floor at 0 for most of the range (30/255 and 24/255 of a dim value round
    # to nothing), so demanding red exceed them by some absolute amount only
    # tests how dim the entry is, not whether it is red.
    _, vd = build_pair(RAMP, "demon")
    lit = [x for x in vd if red5(x) > 0]
    bad = [x for x in lit
           if not (red5(x) > ((x >> 6) & 0x1F) and red5(x) > ((x >> 1) & 0x1F))]
    check(lit and not bad,
          "every entry the veil renders is red-dominant (%d checked)" % len(lit))

    print("── packing ──")
    cold, veiled = build_pair(RAMP, "demon")
    blob = pack(cold, veiled)
    check(len(blob) == 64, "a .pal is exactly 64 bytes (%d)" % len(blob))
    check(struct.unpack(">32H", blob)[:16] == tuple(cold),
          "cold comes first, big-endian")
    check(struct.unpack(">32H", blob)[16:] == tuple(veiled),
          "veiled comes second")

    print("── short palettes are padded, not truncated ──")
    cold, veiled = build_pair([(255, 0, 0), (0, 255, 0)], "demon")
    check(len(cold) == 16 and len(veiled) == 16,
          "a 2-colour source still yields 16 entries (TMEM's slot is 16)")

    print("── determinism ──")
    a = pack(*build_pair(RAMP, "demon"))
    b = pack(*build_pair(RAMP, "demon"))
    check(a == b, "the same input gives byte-identical output")

    print()
    if fails:
        print("%d check(s) FAILED" % len(fails))
        return 1
    print("all checks passed")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", nargs="?", help="indexed (mode P) PNG")
    ap.add_argument("out", nargs="?", help="output .pal (64 bytes)")
    ap.add_argument("--class", dest="cls", default="world", choices=CLASSES,
                    help="material class; see this file's module docstring")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        return selftest()
    if not a.src or not a.out:
        ap.error("need <in.png> <out.pal>, or --selftest")

    palette = read_palette(a.src)
    cold, veiled = build_pair(palette, a.cls)
    with open(a.out, "wb") as fh:
        fh.write(pack(cold, veiled))
    print("veil_palette: %s (%d colours, class %s) -> %s"
          % (a.src, len(palette), a.cls, a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
