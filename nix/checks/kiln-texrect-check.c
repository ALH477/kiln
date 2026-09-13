/* SPDX-License-Identifier: MIT
 *
 * rdpq_sprite_upload + rdpq_texture_rectangle on the host, pixel by pixel.
 *
 * Before these existed a ROM that drew a sprite could not be built for the
 * host at all — examples/streamdb-demo's logo panel is the case that asked.
 * What is pinned, because each is a way to be plausibly wrong:
 *
 *   scale      a 2x2 sprite drawn into 4x4 pixels covers each texel with a
 *              2x2 block, and the unscaled form maps one texel to one pixel
 *   combiner   FLAT discards the texel and draws PRIM, as for triangles; the
 *              rectangle does not helpfully sample anyway
 *   modulate   TEX_FLAT multiplies texel by PRIM
 *   alpha      blender off ignores alpha (the texel's RGB is written); an
 *              alpha compare threshold is what skips it
 *   TMEM       the sprite's bytes are charged to TMEM like any upload
 */
#include <libdragon.h>
#include <kiln_host.h>

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void expect(int x, int y, uint8_t r, uint8_t g, uint8_t b, const char *what)
{
    const surface_t *s = display_get();
    const uint8_t *p = (const uint8_t *)s->buffer + (y * s->width + x) * 4;
    CHECK(p[0] == r && p[1] == g && p[2] == b,
          "%s: pixel (%d,%d) is %d,%d,%d, expected %d,%d,%d",
          what, x, y, p[0], p[1], p[2], r, g, b);
}

int main(void)
{
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    rdpq_init();

    /* A 2x2 RGBA32 .sprite, header big-endian as mksprite writes it:
     *   red    green
     *   blue   white with alpha 0 */
    static uint8_t buf[8 + 16] = {
        0x00, 0x02, 0x00, 0x02, 4, 0, 1, 1,
        0xFF, 0x00, 0x00, 0xFF,   0x00, 0xFF, 0x00, 0xFF,
        0x00, 0x00, 0xFF, 0xFF,   0xFF, 0xFF, 0xFF, 0x00,
    };
    buf[5] = (uint8_t)FMT_RGBA32;
    sprite_t *sp = sprite_load_buf(buf, sizeof buf);
    CHECK(sp->width == 2 && sp->height == 2, "sprite header: %dx%d", sp->width, sp->height);
    CHECK(sprite_get_format(sp) == FMT_RGBA32, "sprite format did not survive the flags byte");

    rdpq_attach_clear(display_get(), NULL);
    rdpq_set_mode_standard();

    CHECK(rdpq_sprite_upload(TILE0, sp, NULL) == 16, "upload did not report 16 bytes");
    CHECK(kiln_host_tmem_used() == 16, "TMEM holds %d bytes, expected 16", kiln_host_tmem_used());

    /* ── scale ── */
    rdpq_mode_combiner(RDPQ_COMBINER_TEX);
    rdpq_texture_rectangle_scaled(TILE0, 10, 10, 14, 14, 0, 0, 2, 2);
    expect(10, 10, 255, 0, 0, "scaled texel 0,0");
    expect(11, 11, 255, 0, 0, "scaled texel 0,0 covers 2x2");
    expect(12, 10, 0, 255, 0, "scaled texel 1,0");
    expect(10, 13, 0, 0, 255, "scaled texel 0,1");
    expect(13, 13, 255, 255, 255, "blender off writes an alpha-0 texel's RGB");
    expect(14, 14, 0, 0, 0, "x1/y1 are exclusive");

    rdpq_texture_rectangle(TILE0, 30, 10, 32, 12, 0, 0);
    expect(30, 10, 255, 0, 0, "unscaled texel 0,0");
    expect(31, 10, 0, 255, 0, "unscaled texel 1,0");
    expect(30, 11, 0, 0, 255, "unscaled texel 0,1");
    expect(32, 10, 0, 0, 0, "unscaled covers exactly the sprite");

    /* ── alpha compare ── */
    rdpq_mode_alphacompare(128);
    rdpq_texture_rectangle_scaled(TILE0, 40, 10, 44, 14, 0, 0, 2, 2);
    expect(43, 13, 0, 0, 0, "alpha compare skips the alpha-0 texel");
    expect(40, 10, 255, 0, 0, "alpha compare keeps opaque texels");
    rdpq_mode_alphacompare(0);

    /* ── combiner ── */
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_set_prim_color(RGBA32(255, 255, 0, 255));
    rdpq_texture_rectangle_scaled(TILE0, 50, 10, 54, 14, 0, 0, 2, 2);
    expect(50, 10, 255, 255, 0, "FLAT discards the texel");

    rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
    rdpq_set_prim_color(RGBA32(255, 128, 255, 255));
    rdpq_texture_rectangle_scaled(TILE0, 60, 10, 64, 14, 0, 0, 2, 2);
    expect(60, 10, 255, 0, 0, "TEX_FLAT red * prim");
    expect(62, 10, 0, 128, 0, "TEX_FLAT green * prim");

    rdpq_detach_show();
    printf("kiln-texrect: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
