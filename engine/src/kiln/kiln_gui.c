/* SPDX-License-Identifier: MIT
 *
 * kiln_gui.c — 2D overlay implementation. See kiln_gui.h.
 */

#include "kiln_gui.h"

#include <stdarg.h>
#include <stdio.h>

/* libdragon colours text through pre-registered *styles*, not per-call. To
 * keep kiln_gui_text's colour parameter honest, colours are interned into style
 * slots on first use and reused thereafter, so a HUD that draws the same few
 * colours every frame registers nothing after frame one. Sixteen slots is
 * plenty for a HUD; past that, calls fall back to white rather than thrashing
 * the style table. */
#define KILN_GUI_STYLES 16
static rdpq_font_t *kiln_font;
static color_t kiln_style_color[KILN_GUI_STYLES];
static int kiln_style_count;

static uint8_t kiln_style_for(color_t c)
{
    for (int i = 0; i < kiln_style_count; i++) {
        color_t s = kiln_style_color[i];
        if (s.r == c.r && s.g == c.g && s.b == c.b && s.a == c.a) return (uint8_t)i;
    }
    if (kiln_style_count >= KILN_GUI_STYLES) return 0; /* palette full: fall back */

    uint8_t id = (uint8_t)kiln_style_count++;
    kiln_style_color[id] = c;
    rdpq_font_style(kiln_font, id, &(rdpq_fontstyle_t){ .color = c });
    return id;
}

void kiln_gui_init(void)
{
    /* Idempotent, and that is load-bearing rather than defensive.
     *
     * kiln_engine_init() already calls this (see kiln_engine.c) — so any ROM
     * that ALSO calls it, which reads as the obvious thing to do next to
     * kiln_input_init(), used to hard-assert at boot:
     *
     *   ASSERTION FAILED: Trying to load already loaded font data
     *   memcmp(fnt->magic, FONT_MAGIC_LOADED, 3)
     *
     * rdpq_font_load_builtin hands back a pointer into a buffer compiled
     * into libdragon and stamps a magic into it, so loading it twice is a
     * hard failure. The ROM dies on a black screen with the LibDragon
     * Inspector up, which looks exactly like "my game renders nothing" and
     * sends you hunting through cameras and near/far planes instead.
     * Three ROMs in this repo had the bug simultaneously.
     *
     * A public init that cannot be called twice — when the engine's own
     * init already called it — is a landmine, so it is defused here rather
     * than documented at each of the call sites that stepped on it. */
    if (kiln_font) return;

    /* The built-in debug font is compiled into libdragon, so the 2D layer has
     * no filesystem dependency at all — a ROM with no DFS image can still draw
     * a HUD. Swap in rdpq_font_load("rom:/x.font64") for a real typeface. */
    kiln_font = rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO);
    rdpq_text_register_font(KILN_GUI_FONT, kiln_font);

    kiln_style_count = 0;
    kiln_style_for(RGBA32(0xFF, 0xFF, 0xFF, 0xFF)); /* style 0 = white default */
}

void kiln_gui_close(void)
{
    /* Deliberately does NOT clear kiln_font. rdpq_font_load_builtin hands back
     * a buffer compiled into libdragon and stamps a magic into it, so it can
     * only ever be loaded once per boot — there is nothing to free, and
     * nulling the pointer here would re-arm the double-load assert for any
     * caller that closed and re-initialised. The asymmetry is the point. */
}

void kiln_gui_begin(void)
{
    /* The one transition out of the 3D pass.
     *
     * rdpq_set_mode_standard resets the combiner/blender to a plain textured
     * or flat-shaded 2D mode. Depth is then explicitly disabled: a HUD is
     * always on top by construction, and leaving the Z compare on would both
     * cost RDP cycles per pixel and let 3D geometry drawn nearer the camera
     * reject HUD pixels. */
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_zbuf(false, false);
    rdpq_mode_alphacompare(0);
}

void kiln_gui_end(void)
{
    rdpq_sync_pipe();
}

void kiln_gui_rect(int x, int y, int w, int h, color_t c)
{
    if (w <= 0 || h <= 0) return;
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_set_prim_color(c);
    rdpq_fill_rectangle(x, y, x + w, y + h);
}

void kiln_gui_panel(int x, int y, int w, int h, color_t fill, color_t border)
{
    if (w <= 0 || h <= 0) return;

    kiln_gui_rect(x, y, w, h, fill);

    /* Four 1px edges rather than a bigger rect underneath: the body may be
     * translucent, and drawing it over a solid border would tint the border. */
    kiln_gui_rect(x, y, w, 1, border);
    kiln_gui_rect(x, y + h - 1, w, 1, border);
    kiln_gui_rect(x, y, 1, h, border);
    kiln_gui_rect(x + w - 1, y, 1, h, border);
}

void kiln_gui_text(int x, int y, color_t c, const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    uint8_t style = kiln_style_for(c);

    rdpq_set_mode_standard();
    rdpq_mode_zbuf(false, false);
    rdpq_text_print(&(rdpq_textparms_t){ .style_id = style },
                    KILN_GUI_FONT, x, y, buf);
}

void kiln_gui_line(int x0, int y0, int x1, int y1, int thickness, color_t c)
{
    if (thickness < 1) thickness = 1;

    const float dx = (float)(x1 - x0);
    const float dy = (float)(y1 - y0);
    const float len2 = dx * dx + dy * dy;

    /* Degenerate: a zero-length line has no perpendicular to offset along, so
     * the quad below would collapse to a point and rdpq would be handed a
     * zero-area triangle. Draw the dot the caller asked for instead — a
     * projected debug point whose two endpoints coincide is a normal case,
     * not a caller error. */
    if (len2 < 1e-6f) {
        kiln_gui_rect(x0 - thickness / 2, y0 - thickness / 2,
                     thickness, thickness, c);
        return;
    }

    /* Perpendicular of length thickness/2, for the quad's two edges.
     * fm_rsqrt would do, but kiln_gui.c deliberately includes no math header
     * and one sqrtf per debug line is not worth the dependency — the RDP is
     * about to shade the pixels either way. */
    const float inv = 0.5f * (float)thickness / __builtin_sqrtf(len2);
    const float px = -dy * inv;
    const float py =  dx * inv;

    /* Flat colour through the shade channel, matching the combiner
     * kiln_gui_begin has already set. Vertex layout is {x, y, r, g, b, a} —
     * the same TRIFMT_SHADE layout as pm_veil's vignette. */
    const float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
    const float a = c.a / 255.0f;

    float v0[] = { (float)x0 + px, (float)y0 + py, r, g, b, a };
    float v1[] = { (float)x1 + px, (float)y1 + py, r, g, b, a };
    float v2[] = { (float)x1 - px, (float)y1 - py, r, g, b, a };
    float v3[] = { (float)x0 - px, (float)y0 - py, r, g, b, a };

    rdpq_set_mode_standard();
    rdpq_mode_zbuf(false, false);
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    /* Blending on, so a translucent debug line reads over the geometry it is
     * describing rather than punching a hole in it. */
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_triangle(&TRIFMT_SHADE, v0, v1, v2);
    rdpq_triangle(&TRIFMT_SHADE, v0, v2, v3);
    /* Put the blender back: kiln_gui_rect/panel/bar downstream of this call
     * expect the mode kiln_gui_begin established, and leaving MULTIPLY set
     * would tint every subsequent opaque HUD rectangle. */
    rdpq_mode_blender(0);
}

void kiln_gui_bar(int x, int y, int w, int h, float frac, color_t fg, color_t bg)
{
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    kiln_gui_rect(x, y, w, h, bg);

    int fill_w = (int)((float)(w - 2) * frac);
    if (fill_w > 0) {
        kiln_gui_rect(x + 1, y + 1, fill_w, h - 2, fg);
    }
}
