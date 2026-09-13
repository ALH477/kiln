/* SPDX-License-Identifier: MIT
 *
 * host_gfx.c — the 2D half of the host backend: a framebuffer, a software
 * rasteriser, and the rdpq/display surface the engine's GUI pass talks to.
 *
 * ── Why software and not OpenGL ────────────────────────────────────────
 * The point of a host build here is a gate that runs inside `nix flake
 * check`, and CLAUDE.md is explicit that screenshot verification is NOT in
 * the gate today because it needs a live Wayland session. A GL backend would
 * carry that problem into the sandbox (a driver, a surface, llvmpipe's own
 * version skew) for a pass that draws axis-aligned rectangles and two
 * triangles per debug line. A software rasteriser has no drivers, is
 * bit-deterministic across machines — which is what makes a committed
 * reference image meaningful — and is closer in shape to the RDP than GL is:
 * the RDP is a coverage rasteriser with a fixed combiner, not a shader
 * pipeline. A window comes later and does not block the gate.
 *
 * ── What it deliberately gets "wrong" ─────────────────────────────────
 * With the blender OFF, the RDP ignores source alpha and writes opaque. So
 * does this. That is not an omission: kiln_gui_panel's own comment says "the
 * body may be translucent", and on console it is not unless the caller turns
 * the blender on. A host that helpfully blended anyway would make a
 * translucent-looking capture of geometry the console draws solid, and the
 * capture is supposed to predict the console.
 *
 * ── What it cannot tell you ───────────────────────────────────────────
 * Fill rate. That is the console's binding constraint and it has no host
 * analogue, so no host capture is evidence that content is affordable. What
 * it can do is COUNT — kiln_host_counters() reports pixels written, which is
 * the quantity the console is actually spending — so the number is available
 * even though the limit is not.
 */
#include <libdragon.h>
#include <kiln_host.h>
#include <kiln_host_font.h>

#include "host_internal.h"


#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── the framebuffer ───────────────────────────────────────────────────── */
#define MAX_W 1024
#define MAX_H 768

static uint8_t   g_fb[MAX_W * MAX_H * 4];   /* RGBA8, row major. Alpha is
                                             * always 255: the N64 framebuffer
                                             * has no transparency, and a host
                                             * buffer cleared to alpha 0 makes
                                             * every captured PNG render over
                                             * whatever the viewer's page
                                             * background is — which read as a
                                             * white screen the first time. */
static uint16_t  g_zb[MAX_W * MAX_H];       /* 0 near .. 65535 far. The 2D pass
                                             * never touches it; the 3D pass
                                             * tests and writes it. */
static surface_t g_color, g_depth;
static int       g_w, g_h;
static int       g_inited;
static uint32_t  g_frame;
static KilnHostCounters g_cnt;

/* ── rdpq mode state ───────────────────────────────────────────────────── */
static rdpq_combiner_t g_comb;
static rdpq_blender_t  g_blend;
static color_t         g_prim  = { 255, 255, 255, 255 };
static color_t         g_fog   = { 0, 0, 0, 255 };
static int             g_attached;

const rdpq_trifmt_t TRIFMT_FILL  = { .pos_offset = 0, .shade_offset = -1,
                                     .tex_offset = -1, .z_offset = -1 };
const rdpq_trifmt_t TRIFMT_SHADE = { .pos_offset = 0, .shade_offset = 2,
                                     .tex_offset = -1, .z_offset = -1 };

/* ── text manifest ─────────────────────────────────────────────────────── */
#define MAX_RUNS 512
typedef struct {
    int     x, y;
    color_t c;
    int     width;      /* measured advance total, in pixels */
    int     missing;    /* codepoints with no glyph in this run */
    char    s[128];
} TextRun;
static TextRun g_runs[MAX_RUNS];
static int     g_nruns;

/* ── plumbing ──────────────────────────────────────────────────────────── */

static inline void put(int x, int y, color_t c)
{
    if (x < 0 || y < 0 || x >= g_w || y >= g_h) return;
    uint8_t *p = &g_fb[(y * g_w + x) * 4];
    if (g_blend == RDPQ_BLENDER_MULTIPLY) {
        /* RDPQ_BLENDER_MULTIPLY is IN_RGB*IN_ALPHA + MEMORY_RGB*(1-IN_ALPHA).
         * Integer maths with a +127 round, so the result does not drift from
         * the console's fixed-point blend by a least significant bit that
         * would then show up in every reference image. */
        unsigned a = c.a, ia = 255u - a;
        p[0] = (uint8_t)((c.r * a + p[0] * ia + 127) / 255);
        p[1] = (uint8_t)((c.g * a + p[1] * ia + 127) / 255);
        p[2] = (uint8_t)((c.b * a + p[2] * ia + 127) / 255);
        p[3] = 255;
    } else {
        /* Blender off: the RDP writes the colour and ignores source alpha. */
        p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 255;
    }
    g_cnt.shaded_px++;
}

/* Opaque black, not zeroed memory. See g_fb's comment. */
static void clear_opaque(void)
{
    for (long i = 0, n = (long)g_w * g_h; i < n; i++) {
        g_fb[i * 4 + 0] = 0; g_fb[i * 4 + 1] = 0;
        g_fb[i * 4 + 2] = 0; g_fb[i * 4 + 3] = 255;
    }
}

void display_init(resolution_t res, int bitdepth, uint32_t num_buffers,
                  int gamma, int filters)
{
    (void)bitdepth; (void)num_buffers; (void)gamma; (void)filters;
    g_w = res.width; g_h = res.height;
    assertf(g_w > 0 && g_h > 0 && g_w <= MAX_W && g_h <= MAX_H,
            "host display_init: %dx%d is outside 1x1..%dx%d", g_w, g_h, MAX_W, MAX_H);
    clear_opaque();
    for (long i = 0, n = (long)g_w * g_h; i < n; i++) g_zb[i] = 0xFFFF;
    g_color = (surface_t){ .flags = 0, .width = (uint16_t)g_w, .height = (uint16_t)g_h,
                           .stride = (uint16_t)(g_w * 4), .buffer = g_fb };
    g_depth = (surface_t){ .flags = 0, .width = (uint16_t)g_w, .height = (uint16_t)g_h,
                           .stride = (uint16_t)(g_w * 2), .buffer = g_zb };
    g_inited = 1;
}

void display_close(void) { g_inited = 0; }
surface_t *display_get(void)
{
    assertf(g_inited, "display_get before display_init");
    /* The console blocks here until the VI releases a buffer, and that is the
     * whole of its frame pacing. A launcher paces and pumps its event queue in
     * the same place, so real time enters the host build at exactly the point
     * it enters the console build rather than at a new one. */
    const KilnHostHooks *h = kiln_host_hooks();
    if (h->vsync) h->vsync(h->ctx);
    return &g_color;
}
surface_t *display_get_zbuf(void) { assertf(g_inited, "display_get_zbuf before display_init"); return &g_depth; }
int display_get_width(void)  { return g_w; }
int display_get_height(void) { return g_h; }

void rdpq_init(void)  { g_frame = 0; }
void rdpq_close(void) { }

void rdpq_attach(surface_t *color, surface_t *z)
{
    (void)z;
    assertf(color != NULL, "rdpq_attach: NULL colour surface");
    g_attached = 1;
    memset(&g_cnt, 0, sizeof g_cnt);
    kiln_host_text_reset();
}
void rdpq_attach_clear(surface_t *color, surface_t *z)
{
    rdpq_attach(color, z);
    clear_opaque();
}
void rdpq_detach(void) { g_attached = 0; }
void rdpq_detach_show(void)
{
    g_attached = 0;
    g_frame++;
    kiln_host_audio_frame();
    const KilnHostHooks *h = kiln_host_hooks();
    if (h->present) h->present(h->ctx, g_fb, g_w, g_h);
}

void rdpq_sync_pipe(void) { }
void rdpq_sync_tile(void) { }
void rspq_wait(void) { }
void rspq_flush(void) { }

void rdpq_set_mode_standard(void)
{
    /* Matches libdragon: a plain 2D mode with the blender off. kiln_gui_begin
     * relies on this resetting the blender, and kiln_gui_line relies on it NOT
     * resetting the combiner it sets afterwards. */
    g_comb  = RDPQ_COMBINER_FLAT;
    g_blend = 0;
}
void rdpq_set_mode_fill(color_t c)      { g_comb = RDPQ_COMBINER_FLAT; g_blend = 0; g_prim = c; }
void rdpq_mode_combiner(rdpq_combiner_t comb) { g_comb = comb; }
rdpq_combiner_t kiln_hostfb_combiner(void) { return g_comb; }
void rdpq_mode_blender(rdpq_blender_t b)      { g_blend = b; }
/* The 3D pass reads these; the 2D pass ignores them, which is exactly what
 * kiln_gui_begin's `rdpq_mode_zbuf(false, false)` is for. */
static int g_ztest, g_zwrite;
void rdpq_mode_zbuf(bool cmp, bool wr)  { g_ztest = cmp; g_zwrite = wr; }
int  kiln_hostfb_ztest(void)  { return g_ztest; }
int  kiln_hostfb_zwrite(void) { return g_zwrite; }
/* Read by the texture rectangle only: every 2D path the engine drew before it
 * was flat and opaque, and kiln_gui_begin sets 0. */
static int g_alphacmp;
void rdpq_mode_alphacompare(int t)      { g_alphacmp = t; }
void rdpq_mode_fog(rdpq_blender_t f)    { (void)f; }
void rdpq_mode_antialias(int m)         { (void)m; }
void rdpq_set_prim_color(color_t c)     { g_prim = c; }
void rdpq_set_fog_color(color_t c)      { g_fog = c; }
color_t kiln_hostfb_fog_color(void)     { return g_fog; }

void rdpq_fill_rectangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    assertf(g_attached, "rdpq_fill_rectangle with nothing attached");
    g_cnt.rects++;
    for (int32_t y = y0; y < y1; y++)
        for (int32_t x = x0; x < x1; x++)
            put((int)x, (int)y, g_prim);
}

void rdpq_texture_rectangle_scaled(rdpq_tile_t tile, float x0, float y0,
                                   float x1, float y1, float s0, float t0,
                                   float s1, float t1)
{
    assertf(g_attached, "rdpq_texture_rectangle with nothing attached");
    assertf(x1 > x0 && y1 > y0, "rdpq_texture_rectangle: empty rectangle "
            "(%g,%g)-(%g,%g)", x0, y0, x1, y1);
    assertf(g_comb == RDPQ_COMBINER_TEX || g_comb == RDPQ_COMBINER_TEX_FLAT ||
            g_comb == RDPQ_COMBINER_FLAT,
            "rdpq_texture_rectangle: a rectangle has no vertex colour, so "
            "combiner %llu (SHADE) has nothing to combine",
            (unsigned long long)g_comb);
    g_cnt.rects++;

    const float dsdx = (s1 - s0) / (x1 - x0), dtdy = (t1 - t0) / (y1 - y0);
    const int ix0 = (int)ceilf(x0), iy0 = (int)ceilf(y0);
    const int ix1 = (int)ceilf(x1), iy1 = (int)ceilf(y1);
    for (int y = iy0; y < iy1; y++) {
        for (int x = ix0; x < ix1; x++) {
            color_t c = g_prim;
            if (g_comb != RDPQ_COMBINER_FLAT) {
                const float s = s0 + ((float)x - x0) * dsdx;
                const float t = t0 + ((float)y - y0) * dtdy;
                assertf(kiln_hosttex_sample((int)tile, s, t, &c),
                        "rdpq_texture_rectangle: TILE%d has no texture uploaded",
                        (int)tile);
                if (g_comb == RDPQ_COMBINER_TEX_FLAT) {
                    c.r = (uint8_t)((c.r * g_prim.r + 127) / 255);
                    c.g = (uint8_t)((c.g * g_prim.g + 127) / 255);
                    c.b = (uint8_t)((c.b * g_prim.b + 127) / 255);
                    c.a = (uint8_t)((c.a * g_prim.a + 127) / 255);
                }
            }
            if (g_alphacmp > 0 && c.a < g_alphacmp) continue;
            put(x, y, c);
        }
    }
}

/* ── triangles ─────────────────────────────────────────────────────────
 * Barycentric, with a top-left fill rule so two triangles sharing an edge
 * neither double-shade it (visible with the blender on, which is exactly how
 * kiln_gui_line draws its quad) nor leave a seam. */
static inline float edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

void rdpq_triangle(const rdpq_trifmt_t *fmt, const float *v0,
                   const float *v1, const float *v2)
{
    assertf(g_attached, "rdpq_triangle with nothing attached");
    assertf(fmt != NULL, "rdpq_triangle: NULL format");
    assertf(fmt->pos_offset >= 0, "rdpq_triangle: format has no position");
    g_cnt.tris++;

    const int po = fmt->pos_offset, so = fmt->shade_offset;
    float x0 = v0[po], y0 = v0[po + 1];
    float x1 = v1[po], y1 = v1[po + 1];
    float x2 = v2[po], y2 = v2[po + 1];

    float area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0.0f) return;                 /* degenerate: nothing to shade */
    if (area < 0.0f) {                        /* accept either winding */
        float tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty;
        const float *tv = v1; v1 = v2; v2 = tv;
        area = -area;
    }

    int minx = (int)floorf(fminf(x0, fminf(x1, x2)));
    int maxx = (int)ceilf (fmaxf(x0, fmaxf(x1, x2)));
    int miny = (int)floorf(fminf(y0, fminf(y1, y2)));
    int maxy = (int)ceilf (fmaxf(y0, fmaxf(y1, y2)));
    if (minx < 0)   minx = 0;
    if (miny < 0)   miny = 0;
    if (maxx > g_w) maxx = g_w;
    if (maxy > g_h) maxy = g_h;

    const float inv = 1.0f / area;
    for (int y = miny; y < maxy; y++) {
        for (int x = minx; x < maxx; x++) {
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = edge(x1, y1, x2, y2, px, py);
            float w1 = edge(x2, y2, x0, y0, px, py);
            float w2 = edge(x0, y0, x1, y1, px, py);
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            color_t c = g_prim;
            if (g_comb == RDPQ_COMBINER_SHADE && so >= 0) {
                float a = w0 * inv, b = w1 * inv, g = w2 * inv;
                if (fmt->shade_flat) { a = 1.0f; b = g = 0.0f; }
                float r = a * v0[so + 0] + b * v1[so + 0] + g * v2[so + 0];
                float gg= a * v0[so + 1] + b * v1[so + 1] + g * v2[so + 1];
                float bb= a * v0[so + 2] + b * v1[so + 2] + g * v2[so + 2];
                float aa= a * v0[so + 3] + b * v1[so + 3] + g * v2[so + 3];
                c.r = (uint8_t)(fminf(fmaxf(r, 0.0f), 1.0f) * 255.0f + 0.5f);
                c.g = (uint8_t)(fminf(fmaxf(gg,0.0f), 1.0f) * 255.0f + 0.5f);
                c.b = (uint8_t)(fminf(fmaxf(bb,0.0f), 1.0f) * 255.0f + 0.5f);
                c.a = (uint8_t)(fminf(fmaxf(aa,0.0f), 1.0f) * 255.0f + 0.5f);
            }
            put(x, y, c);
        }
    }
}

/* ── the font ──────────────────────────────────────────────────────────
 * One font, libdragon's builtin, so rdpq_font_load_builtin hands back a token
 * rather than a parsed object. rdpq_font_load of a real .font64 is not
 * supported and says so: silently substituting the builtin would make a ROM
 * that ships a typeface render in the wrong one, which is a difference nobody
 * would look for. */
struct rdpq_font_s {
    char    magic[3];   /* stamped "FNL" so kiln_gui's double-load guard sees
                         * what it sees on console */
    uint8_t which;
    rdpq_fontstyle_t styles[64];
    uint8_t nstyles;
};
static struct rdpq_font_s g_font;
static struct rdpq_font_s *g_registered[8];

rdpq_font_t *rdpq_font_load_builtin(rdpq_font_builtin_t which)
{
    assertf(which == FONT_BUILTIN_DEBUG_MONO,
            "host: only FONT_BUILTIN_DEBUG_MONO is available (asked for %d)", which);
    /* libdragon returns a pointer into a buffer compiled into the library and
     * stamps a magic into it, so loading twice is a hard assert. kiln_gui.c
     * carries a whole comment about the three ROMs that hit it. Reproduce it:
     * a host build that tolerated the double load would let that bug back in
     * on console. */
    assertf(memcmp(g_font.magic, FONT_MAGIC_LOADED, 3) != 0,
            "Trying to load already loaded font data");
    memcpy(g_font.magic, FONT_MAGIC_LOADED, 3);
    g_font.which = (uint8_t)which;
    g_font.styles[0] = (rdpq_fontstyle_t){ .color = { 255,255,255,255 },
                                           .outline_color = { 0,0,0,255 } };
    g_font.nstyles = 1;
    return &g_font;
}

rdpq_font_t *rdpq_font_load(const char *path)
{
    assertf(0, "host: rdpq_font_load(\"%s\") is not implemented. Only the "
               "builtin debug font is available; see plat/host/src/host_gfx.c.",
            path ? path : "(null)");
    return NULL;
}

void rdpq_font_style(rdpq_font_t *font, uint8_t id, const rdpq_fontstyle_t *style)
{
    assertf(font != NULL && style != NULL, "rdpq_font_style: NULL");
    assertf(id < 64, "rdpq_font_style: style %d out of range", id);
    font->styles[id] = *style;
    if (id >= font->nstyles) font->nstyles = (uint8_t)(id + 1);
}

void rdpq_text_register_font(uint8_t id, rdpq_font_t *font)
{
    assertf(id < 8, "rdpq_text_register_font: id %d out of range", id);
    g_registered[id] = font;
}

static const KilnFontGlyph *glyph_for(unsigned cp)
{
    if (cp < KILN_FONT_FIRST_CP || cp > KILN_FONT_LAST_CP) return NULL;
    const KilnFontGlyph *g = &kiln_font_glyphs[cp - KILN_FONT_FIRST_CP];
    return (g->advance == 0 && g->w == 0) ? NULL : g;
}

int rdpq_text_print(const rdpq_textparms_t *parms, uint8_t font_id,
                    float x0, float y0, const char *utf8_text)
{
    assertf(font_id < 8 && g_registered[font_id],
            "rdpq_text_print: font %d not registered", font_id);
    assertf(utf8_text != NULL, "rdpq_text_print: NULL text");
    struct rdpq_font_s *f = g_registered[font_id];
    uint8_t sid = parms ? parms->style_id : 0;
    assertf(sid < f->nstyles, "rdpq_text_print: style %d not registered", sid);
    const rdpq_fontstyle_t *st = &f->styles[sid];

    /* y0 is the BASELINE, and each glyph's yoff is relative to it (the
     * builtin font's yoff values are negative, spanning ascent above the
     * baseline). Unverified against a console capture — when the 3D tier
     * lands and a real ROM can be shot on both, this is the first thing to
     * check, because a whole-HUD vertical shift would look like correct
     * output in a host-only reference. */
    int cursor = (int)x0;
    const int baseline = (int)y0;
    int missing = 0, run_start = cursor;

    for (const unsigned char *p = (const unsigned char *)utf8_text; *p; p++) {
        const KilnFontGlyph *g = glyph_for(*p);
        if (!g) { missing++; g_cnt.missing++; cursor += KILN_FONT_SPACE_WIDTH; continue; }
        for (int gy = 0; gy < g->h; gy++) {
            for (int gx = 0; gx < g->w; gx++) {
                uint8_t cov = kiln_font_bits[g->bits + gy * g->w + gx];
                if (!cov) continue;
                put(cursor + g->xoff + gx, baseline + g->yoff + gy,
                    cov == 1 ? st->color : st->outline_color);
            }
        }
        if (g->w) g_cnt.glyphs++;
        cursor += g->advance;
    }

    g_cnt.text_runs++;
    if (g_nruns < MAX_RUNS) {
        TextRun *r = &g_runs[g_nruns++];
        r->x = (int)x0; r->y = baseline; r->c = st->color;
        r->width = cursor - run_start; r->missing = missing;
        snprintf(r->s, sizeof r->s, "%s", utf8_text);
    }
    return cursor - run_start;
}

/* ── what host_t3d.c borrows ───────────────────────────────────────────── */

int kiln_hostfb_w(void) { return g_w; }
int kiln_hostfb_h(void) { return g_h; }
int kiln_hostfb_attached(void) { return g_attached; }
void kiln_hostfb_put(int x, int y, color_t c) { put(x, y, c); }

void kiln_hostfb_put_z(int x, int y, uint16_t z, color_t c, int test, int write)
{
    if (x < 0 || y < 0 || x >= g_w || y >= g_h) return;
    uint16_t *zp = &g_zb[y * g_w + x];
    /* Less-than, matching the RDP's default depth compare. Equal fails, so
     * coplanar geometry drawn later does not win — which is why a decal needs
     * test-without-write rather than a bias. */
    if (test && z >= *zp) return;
    if (write) *zp = z;
    put(x, y, c);
}

void kiln_hostfb_clear_color(color_t c)
{
    for (long i = 0, n = (long)g_w * g_h; i < n; i++) {
        g_fb[i * 4 + 0] = c.r; g_fb[i * 4 + 1] = c.g;
        g_fb[i * 4 + 2] = c.b; g_fb[i * 4 + 3] = 255;
    }
}

void kiln_hostfb_clear_depth(void)
{
    for (long i = 0, n = (long)g_w * g_h; i < n; i++) g_zb[i] = 0xFFFF;
}

/* ── the host control surface ──────────────────────────────────────────── */

uint32_t kiln_host_frame(void) { return g_frame; }
const KilnHostCounters *kiln_host_counters(void) { return &g_cnt; }
void kiln_host_text_reset(void) { g_nruns = 0; }

int kiln_host_text_manifest(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "# kiln host text manifest: one line per rdpq_text_print\n");
    fprintf(fp, "# x y  rgba  width missing  \"text\"\n");
    for (int i = 0; i < g_nruns; i++) {
        const TextRun *r = &g_runs[i];
        fprintf(fp, "%4d %4d  %02x%02x%02x%02x  %4d %d  \"%s\"\n",
                r->x, r->y, r->c.r, r->c.g, r->c.b, r->c.a,
                r->width, r->missing, r->s);
    }
    fclose(fp);
    return 0;
}

void kiln_host_stats(FILE *out, int top)
{
    /* Deliberately the same quantities tools/n64-shot.sh prints, so a host
     * capture and a console capture are read the same way — CLAUDE.md's
     * "trust the pixel statistics, not your eyes" applies to both. */
    long total = (long)g_w * g_h, nonblack = 0;
    typedef struct { uint32_t rgb; long n; } Bin;
    static Bin bins[4096];
    int nbins = 0;
    for (long i = 0; i < total; i++) {
        uint32_t rgb = ((uint32_t)g_fb[i*4] << 16) | ((uint32_t)g_fb[i*4+1] << 8)
                     | g_fb[i*4+2];
        if (rgb) nonblack++;
        int j = 0;
        for (; j < nbins; j++) if (bins[j].rgb == rgb) { bins[j].n++; break; }
        if (j == nbins && nbins < (int)(sizeof bins / sizeof bins[0]))
            bins[nbins++] = (Bin){ rgb, 1 };
    }
    fprintf(out, "%dx%d  non-black %ld/%ld (%.1f%%)  colours %d\n",
            g_w, g_h, nonblack, total, 100.0 * (double)nonblack / (double)total, nbins);
    for (int k = 0; k < top; k++) {
        int best = -1;
        for (int j = 0; j < nbins; j++)
            if (bins[j].n > 0 && (best < 0 || bins[j].n > bins[best].n)) best = j;
        if (best < 0) break;
        fprintf(out, "  #%06x  %ld (%.1f%%)\n", bins[best].rgb, bins[best].n,
                100.0 * (double)bins[best].n / (double)total);
        bins[best].n = 0;
    }
    fprintf(out, "  rects %u  tris %u  text %u  glyphs %u  missing %u  "
                 "pixels-written %llu\n",
            g_cnt.rects, g_cnt.tris, g_cnt.text_runs, g_cnt.glyphs, g_cnt.missing,
            (unsigned long long)g_cnt.shaded_px);
}

/* host_png.c */
int kiln_host_write_png(const char *path, const uint8_t *rgba, int w, int h);

int kiln_host_capture(const char *path)
{
    assertf(g_inited, "kiln_host_capture before display_init");
    return kiln_host_write_png(path, g_fb, g_w, g_h);
}
