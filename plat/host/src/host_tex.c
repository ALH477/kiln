/* SPDX-License-Identifier: MIT
 *
 * host_tex.c — TMEM, the TLUT, and texture sampling on the host.
 *
 * ── TMEM is the point of this file ────────────────────────────────────
 * The RDP samples from TMEM: 4 KB of on-chip memory a texture must be DMA'd
 * into before it can be drawn. Overflowing it does not fail — it WRAPS, and
 * you get a texture assembled out of whatever else happened to be resident.
 * There is no diagnostic on hardware at all.
 *
 * So this tracks occupancy and asserts. That is one of the very few console
 * limits a host build can genuinely check, and it is the direction CLAUDE.md
 * asks for: a host that is STRICTER than the console rather than laxer. It
 * cannot see fill rate, but it can see this.
 *
 * ── The combiner decides, and that is not a detail ────────────────────
 * Uploading a texture does not make it visible. The RSP emits texture
 * coordinates when T3D_FLAG_TEXTURED is set, and then the RDP's COLOR COMBINER
 * decides whether the sampled texel reaches the framebuffer. With
 * RDPQ_COMBINER_SHADE — which is what kiln_scene_begin sets, every frame — the
 * output is vertex colour and the texel is discarded.
 *
 * This shim reproduces that faithfully rather than helpfully sampling anyway,
 * because "uploaded but never sampled" is a real state this codebase is in and
 * hiding it would be worse than useless. See kiln-voxmesh's check for what
 * that currently means for Forge.
 */
#include <libdragon.h>
#include <math.h>
#include <string.h>

#include "host_internal.h"

typedef struct {
    surface_t       surf;
    rdpq_texparms_t parms;
    int             bound;
    int             tmem_bytes;
} Tile;

static Tile        g_tiles[8];
static uint16_t    g_tlut[256];
static int         g_tlut_loaded;
static rdpq_tlut_t g_tlut_mode = TLUT_NONE;
static int         g_tmem_used;

int kiln_host_tmem_used(void) { return g_tmem_used; }

int rdpq_tex_upload(rdpq_tile_t tile, const surface_t *tex,
                    const rdpq_texparms_t *parms)
{
    assertf(tex != NULL, "rdpq_tex_upload: NULL surface");
    assertf((int)tile >= 0 && (int)tile < 8, "rdpq_tex_upload: tile %d", (int)tile);
    assertf(tex->buffer != NULL, "rdpq_tex_upload: surface has no buffer");
    assertf(tex->width > 0 && tex->height > 0,
            "rdpq_tex_upload: %dx%d surface", tex->width, tex->height);

    const int bytes = tex->stride * tex->height;

    /* Recharge the accounting for this tile rather than accumulating: a ROM
     * that re-uploads the same atlas every frame is not consuming more TMEM
     * each time. */
    g_tmem_used -= g_tiles[tile].tmem_bytes;
    g_tmem_used += bytes;

    assertf(g_tmem_used <= TMEM_BYTES,
            "TMEM overflow: %d bytes resident after uploading a %dx%d %s "
            "texture (%d bytes) to TILE%d, against a %d-byte budget. On console "
            "this WRAPS with no diagnostic and you get a texture built out of "
            "whatever else was loaded.",
            g_tmem_used, tex->width, tex->height,
            surface_get_format(tex) == FMT_CI4  ? "CI4"  :
            surface_get_format(tex) == FMT_CI8  ? "CI8"  :
            surface_get_format(tex) == FMT_RGBA16 ? "RGBA16" :
            surface_get_format(tex) == FMT_RGBA32 ? "RGBA32" : "other",
            bytes, (int)tile, TMEM_BYTES);

    g_tiles[tile].surf = *tex;
    g_tiles[tile].parms = parms ? *parms : (rdpq_texparms_t){ 0 };
    g_tiles[tile].bound = 1;
    g_tiles[tile].tmem_bytes = bytes;
    return bytes;
}

void rdpq_tex_upload_tlut(uint16_t *tlut, int color_idx, int num_colors)
{
    assertf(tlut != NULL, "rdpq_tex_upload_tlut: NULL palette");
    assertf(color_idx >= 0 && num_colors > 0 && color_idx + num_colors <= 256,
            "rdpq_tex_upload_tlut: %d colours at %d overruns the 256-entry TLUT",
            num_colors, color_idx);
    memcpy(&g_tlut[color_idx], tlut, (size_t)num_colors * sizeof(uint16_t));
    g_tlut_loaded = 1;
}

void rdpq_mode_tlut(rdpq_tlut_t tlut)
{
    assertf(tlut == TLUT_NONE || tlut == TLUT_RGBA16 || tlut == TLUT_IA16,
            "rdpq_mode_tlut: invalid TLUT type %d", (int)tlut);
    g_tlut_mode = tlut;
}

void rdpq_mode_persp(bool p)   { (void)p; }
void rdpq_mode_filter(int f)   { (void)f; }
void rdpq_mode_dithering(int d){ (void)d; }

void rdpq_set_lookup_address(uint8_t index, void *rdram_addr)
{
    /* On console this points one of the RDP's lookup slots at RDRAM, which is
     * how kiln_texanim swaps a palette without re-uploading the texture. The
     * host has no indirection to set up — the pointer IS the address — so this
     * is recorded and nothing else. */
    (void)index; (void)rdram_addr;
}

/* ── sampling ─────────────────────────────────────────────────────────── */

static inline color_t rgba16_to_color(uint16_t p)
{
    /* RGBA 5551, replicating the high bits into the low ones so 31 maps to
     * 255 — the same expansion RGBA16() does. */
    const int r = (p >> 11) & 0x1F, g = (p >> 6) & 0x1F, b = (p >> 1) & 0x1F;
    return (color_t){ (uint8_t)((r << 3) | (r >> 2)),
                      (uint8_t)((g << 3) | (g >> 2)),
                      (uint8_t)((b << 3) | (b >> 2)),
                      (uint8_t)((p & 1) ? 255 : 0) };
}

static inline int wrap_coord(float c, int size, float repeats)
{
    int i = (int)floorf(c);
    if (repeats >= (float)REPEAT_INFINITE) {
        i %= size;
        if (i < 0) i += size;
        return i;
    }
    const int limit = (int)(repeats * (float)size);
    if (i < 0) i = 0;
    if (i >= limit) i = limit - 1;
    i %= size;
    if (i < 0) i += size;
    return i;
}

int kiln_hosttex_sample(int tile, float s, float t, color_t *out)
{
    if (tile < 0 || tile >= 8 || !g_tiles[tile].bound) return 0;
    const Tile *T = &g_tiles[tile];
    const uint8_t *px = (const uint8_t *)T->surf.buffer;
    const int x = wrap_coord(s, T->surf.width,  T->parms.s.repeats);
    const int y = wrap_coord(t, T->surf.height, T->parms.t.repeats);

    switch (surface_get_format(&T->surf)) {
    case FMT_CI4: {
        const uint8_t byte = px[y * T->surf.stride + x / 2];
        const int idx = (x & 1) ? (byte & 0x0F) : (byte >> 4);
        if (g_tlut_mode == TLUT_NONE || !g_tlut_loaded) {
            /* No palette: the RDP would sample the raw index as intensity.
             * Reproduce that rather than guessing a colour. */
            const uint8_t v = (uint8_t)(idx * 17);
            *out = (color_t){ v, v, v, 255 };
            return 1;
        }
        *out = rgba16_to_color(g_tlut[T->parms.palette * 16 + idx]);
        return 1;
    }
    case FMT_CI8: {
        const int idx = px[y * T->surf.stride + x];
        if (g_tlut_mode == TLUT_NONE || !g_tlut_loaded) {
            *out = (color_t){ (uint8_t)idx, (uint8_t)idx, (uint8_t)idx, 255 };
            return 1;
        }
        *out = rgba16_to_color(g_tlut[idx]);
        return 1;
    }
    case FMT_RGBA16: {
        const uint8_t *q = &px[y * T->surf.stride + x * 2];
        *out = rgba16_to_color((uint16_t)((q[0] << 8) | q[1]));
        return 1;
    }
    case FMT_RGBA32: {
        const uint8_t *q = &px[y * T->surf.stride + x * 4];
        *out = (color_t){ q[0], q[1], q[2], q[3] };
        return 1;
    }
    case FMT_I8: {
        const uint8_t v = px[y * T->surf.stride + x];
        *out = (color_t){ v, v, v, 255 };
        return 1;
    }
    default:
        assertf(0, "kiln_hosttex_sample: texture format %#x is not implemented. "
                   "Add it to plat/host/src/host_tex.c rather than letting it "
                   "sample as something else.",
                (unsigned)surface_get_format(&T->surf));
        return 0;
    }
}
