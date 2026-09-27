/* SPDX-License-Identifier: MIT
 *
 * Draws a HUD with the REAL kiln_gui.c, through the host 2D backend, and
 * captures it. Every fig_gui primitive is exercised, because each one takes
 * a different path through the rdpq shim: rect and bar are fill_rectangle
 * with COMBINER_FLAT, panel is four one-pixel rects over a body, line is two
 * COMBINER_SHADE triangles with the blender on, and text is the extracted
 * builtin font.
 *
 * The frame is deliberately shaped like a real overlay rather than a test
 * card: the failure this is meant to catch is "the HUD moved", and a grid of
 * squares would not show that.
 */
#include <kiln_gui.h>
#include <kiln_host.h>
#include <libdragon.h>
#include <stdio.h>
#include <string.h>

#define W 320
#define H 240

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void draw_hud(void)
{
    const color_t cyan  = RGBA32(0x00, 0xF5, 0xD4, 0xFF);
    const color_t white = RGBA32(0xFF, 0xFF, 0xFF, 0xFF);
    const color_t red   = RGBA32(0xE0, 0x1A, 0x20, 0xFF);
    const color_t dim   = RGBA32(0x60, 0x60, 0x70, 0xFF);
    const color_t panel = RGBA32(0x10, 0x12, 0x18, 0xFF);

    fig_gui_begin();

    /* A title bar and a body panel: two rects and a bordered panel. */
    fig_gui_rect(0, 0, W, 12, panel);
    fig_gui_text(6, 9, cyan, "KILN GUI");
    fig_gui_text(W - 78, 9, dim, "host 2D");

    fig_gui_panel(8, 20, 150, 64, panel, cyan);
    fig_gui_text(14, 34, white, "air  %d%%", 62);
    fig_gui_bar(14, 40, 130, 8, 0.62f, cyan, dim);
    fig_gui_text(14, 60, white, "clip %d/%d", 3, 512);
    fig_gui_text(14, 74, red,   "veil UP");

    /* Glyph coverage, and the character CLAUDE.md said was missing. */
    fig_gui_text(8, 104, white, "0O@ 8B .-_ 2@5.4");
    fig_gui_text(8, 118, dim,   "abcdefghijklmnopqrstuvwxyz");
    fig_gui_text(8, 132, dim,   "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    fig_gui_text(8, 146, dim,   "0123456789 !\"#$%%&'()*+,-./");

    /* Lines: the only COMBINER_SHADE path, and the only blended one. Drawn
     * as a fan so a winding or fill-rule mistake shows as a wedge. */
    for (int i = 0; i < 8; i++)
        fig_gui_line(240, 180, 240 + (i - 4) * 16, 210, 1 + i % 3,
                      RGBA32(0xFF, (uint8_t)(0x40 + i * 24), 0x20, 0xFF));

    /* A translucent rect. With the blender off — which is where fig_gui_begin
     * leaves it — the RDP ignores source alpha and writes opaque. This is here
     * so the reference image records that, rather than someone later "fixing"
     * the host to blend and quietly diverging from the console. */
    fig_gui_rect(170, 20, 60, 20, RGBA32(0x00, 0xFF, 0x00, 0x40));
    fig_gui_text(174, 34, white, "alpha40");

    fig_gui_end();
}

int main(int argc, char **argv)
{
    const char *png      = argc > 1 ? argv[1] : "hud.png";
    const char *manifest = argc > 2 ? argv[2] : "hud.txt";

    resolution_t res = RESOLUTION_320x240;
    display_init(res, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    rdpq_init();
    fig_gui_init();

    /* fig_gui_init is documented as idempotent and that is load-bearing:
     * calling it twice used to hard-assert at boot in three ROMs. The host
     * font shim reproduces libdragon's double-load assert, so this line is
     * the regression test for the guard as well as a call. */
    fig_gui_init();

    rdpq_attach(display_get(), display_get_zbuf());
    draw_hud();
    rdpq_detach_show();

    CHECK(fig_host_frame() == 1, "frame counter is %u, expected 1",
          fig_host_frame());

    const FigHostCounters *c = fig_host_counters();
    printf("  counters: rects %u tris %u text %u glyphs %u missing %u px %llu\n",
           c->rects, c->tris, c->text_runs, c->glyphs, c->missing,
           (unsigned long long)c->shaded_px);

    /* fig_gui_line draws two triangles per call, 8 calls. */
    CHECK(c->tris == 16, "expected 16 triangles from 8 lines, got %u", c->tris);
    /* Every glyph in the frame is printable ASCII, so none may be missing —
     * this is the assertion that would have caught the '@' claim being wrong
     * in the other direction too. */
    CHECK(c->missing == 0, "%u codepoints had no glyph", c->missing);
    CHECK(c->glyphs > 100, "only %u glyphs rasterised", c->glyphs);
    CHECK(c->shaded_px > 0, "nothing was drawn");

    fig_host_stats(stdout, 6);

    CHECK(fig_host_capture(png) == 0, "could not write %s", png);
    CHECK(fig_host_text_manifest(manifest) == 0, "could not write %s", manifest);

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("host 2D pass rendered a HUD through the real kiln_gui.c\n");
    return 0;
}
