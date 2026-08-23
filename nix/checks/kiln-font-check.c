/* SPDX-License-Identifier: MIT
 *
 * Invariants of the extracted builtin font that a diff of the generated
 * header cannot catch. tools/font_extract.py regenerating byte-identically
 * proves it was not hand-edited; this proves the bytes mean what the host 2D
 * layer assumes about them.
 */
#include <kiln_host_font.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const KilnFontGlyph *G(int cp)
{
    if (cp < KILN_FONT_FIRST_CP || cp > KILN_FONT_LAST_CP) return NULL;
    return &kiln_font_glyphs[cp - KILN_FONT_FIRST_CP];
}

int main(void)
{
    /* ── monospaced, which is what makes host text extents exact ──
     * kiln_gui_text's layout on the host is arithmetic, not measurement:
     * width = advance * length. That is only true while every glyph shares
     * one advance and the font carries no kerning (the extractor asserts
     * num_kerning == 0 separately). If libdragon ever swaps the builtin for
     * a proportional font this must fail, because every host HUD reference
     * image silently shifts. */
    for (int cp = 0x20; cp <= 0x7E; cp++) {
        const KilnFontGlyph *g = G(cp);
        CHECK(g != NULL, "printable ASCII %#x has no table entry", cp);
        if (!g) continue;
        CHECK(g->advance == KILN_FONT_SPACE_WIDTH,
              "cp %#x '%c' advance %d, expected %d — font is not monospaced",
              cp, cp, g->advance, KILN_FONT_SPACE_WIDTH);
    }

    /* Space draws nothing and still advances. */
    CHECK(G(0x20)->w == 0 && G(0x20)->h == 0, "space has a bitmap");
    CHECK(G(0x20)->advance == KILN_FONT_SPACE_WIDTH, "space does not advance");

    /* Every other printable glyph has pixels, and at least one FILL pixel —
     * a glyph that is all outline and no fill decoded the wrong 2-bit layer,
     * which is exactly the failure that produces recognisable garbage. */
    for (int cp = 0x21; cp <= 0x7E; cp++) {
        const KilnFontGlyph *g = G(cp);
        if (!g || (g->w == 0 && g->h == 0)) { CHECK(0, "cp %#x '%c' is blank", cp, cp); continue; }
        int fill = 0, outline = 0, n = g->w * g->h;
        for (int i = 0; i < n; i++) {
            uint8_t v = kiln_font_bits[g->bits + i];
            CHECK(v <= 2, "cp %#x coverage %d out of range", cp, v);
            if (v == 1) fill++;
            if (v == 2) outline++;
        }
        CHECK(fill > 0, "cp %#x '%c' has no fill pixels — wrong 2bpp layer?", cp, cp);
        CHECK(outline > 0, "cp %#x '%c' has no outline pixels", cp, cp);
    }

    /* ── '@' is not '0' ──
     * CLAUDE.md carried a "hard-won fact" that FONT_BUILTIN_DEBUG_MONO has no
     * '@' glyph and draws it as '0', so a debug label reading "2@5.4" comes
     * out as "205.4" — and told everyone to avoid '@' in overlay text. It is
     * not true of the pinned libdragon: both glyphs exist and differ. Pinned
     * here so that if a libdragon bump ever makes it true, it fails loudly
     * instead of being rediscovered by someone reading a coordinate off a
     * screenshot. */
    const KilnFontGlyph *at = G('@'), *zero = G('0');
    CHECK(at->w > 0 && at->h > 0, "'@' has no glyph");
    CHECK(zero->w > 0 && zero->h > 0, "'0' has no glyph");
    if (at->w == zero->w && at->h == zero->h) {
        CHECK(memcmp(&kiln_font_bits[at->bits], &kiln_font_bits[zero->bits],
                     (size_t)at->w * at->h) != 0,
              "'@' and '0' have identical bitmaps");
    }

    /* Vertical metrics the rasteriser positions against. */
    CHECK(KILN_FONT_ASCENT > 0, "ascent %d", KILN_FONT_ASCENT);
    CHECK(KILN_FONT_DESCENT <= 0, "descent %d should be <= 0", KILN_FONT_DESCENT);

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("builtin font table: 0x20-0x7E all present, monospaced at %d px, "
           "fill+outline layers intact, '@' != '0'\n", KILN_FONT_SPACE_WIDTH);
    return 0;
}
