/* SPDX-License-Identifier: MIT
 *
 * kiln_voxmesh.c — see kiln_voxmesh.h. Packing, batching, drawing, and the
 * runtime CI4 atlas. No decisions about what geometry exists are made here.
 */
#include <libdragon.h>
#include <string.h>

#include "kiln_voxmesh.h"

/* ── Arena ─────────────────────────────────────────────────────────────*/

void kiln_voxmesh_arena_init(KilnVoxMeshArena *a, T3DVertPacked *base,
                            uint32_t entries)
{
    a->base = base;
    a->capacity = entries;
    a->used = 0;
}

void kiln_voxmesh_arena_reset(KilnVoxMeshArena *a) { a->used = 0; }

uint32_t kiln_voxmesh_arena_used_pct(const KilnVoxMeshArena *a)
{
    if (!a->capacity) return 100;
    return (uint32_t)(((uint64_t)a->used * 100) / a->capacity);
}

/* ── Vertex packing ────────────────────────────────────────────────────*/

/* Per-direction brightness. Top brightest, bottom darkest, the four sides in
 * two pairs so opposite walls of a corridor do not read as the same surface.
 * These are the values that make an unlit cube legible; see the header. */
static const uint8_t DIR_SHADE[KILN_VOXEL_DIRS] = {
    /* XP */ 0xB4, /* XN */ 0x8C,
    /* YP */ 0xFF, /* YN */ 0x50,
    /* ZP */ 0xA0, /* ZN */ 0x78,
};

/* The four corners of a face, as (u, v) offsets in the face's own tangent
 * basis, wound counter-clockwise seen from OUTSIDE. Two tables because the
 * negative-facing directions need the opposite winding or every other face of
 * every block is backface-culled — which looks exactly like a hole. */
static const int CORNER_U[4] = { 0, 1, 1, 0 };
static const int CORNER_V[4] = { 0, 0, 1, 1 };

static void pack_vert(T3DVertPacked *dst, int idx, const int pos[3],
                      int16_t s, int16_t t, uint32_t rgba, uint16_t norm)
{
    /* T3DVertPacked holds TWO vertices; idx picks the A or B half. */
    T3DVertPacked *v = &dst[idx >> 1];
    if ((idx & 1) == 0) {
        v->posA[0] = (int16_t)pos[0];
        v->posA[1] = (int16_t)pos[1];
        v->posA[2] = (int16_t)pos[2];
        v->normA = norm;
        v->rgbaA = rgba;
        v->stA[0] = s;
        v->stA[1] = t;
    } else {
        v->posB[0] = (int16_t)pos[0];
        v->posB[1] = (int16_t)pos[1];
        v->posB[2] = (int16_t)pos[2];
        v->normB = norm;
        v->rgbaB = rgba;
        v->stB[0] = s;
        v->stB[1] = t;
    }
}

int kiln_voxmesh_build(const KilnVoxelWorld *w, int slot,
                      const KilnVoxelQuad *quads, uint32_t quad_count,
                      KilnVoxMeshArena *arena, int atlas_tiles,
                      KilnVoxMesh *out)
{
    (void)slot;
    memset(out, 0, sizeof *out);
    if (!quad_count) return 0;

    /* Two T3DVertPacked entries per quad (4 vertices). */
    uint32_t need = quad_count * 2;
    if (arena->used + need > arena->capacity) return -1;

    T3DVertPacked *base = arena->base + arena->used;
    arena->used += need;

    const int B = KILN_VOXEL_BLOCK_UNITS;
    const int tile_px = KILN_VOXATLAS_TILE;
    if (atlas_tiles < 1) atlas_tiles = 1;

    for (uint32_t q = 0; q < quad_count; q++) {
        const KilnVoxelQuad *qd = &quads[q];
        int axes[3], sgn;
        kiln_voxel_dir_axes(qd->dir, axes, &sgn);
        const int au = axes[0], av = axes[1], an = axes[2];

        /* The face plane: the block's low corner on the normal axis, plus one
         * block when the face points positive. */
        int origin[3] = { qd->x, qd->y, qd->z };
        int plane = origin[an] + (sgn > 0 ? 1 : 0);

        /* Atlas tile for this block type. Type 1 is tile 0, so air needs none. */
        int tile = (qd->block >= 1) ? (qd->block - 1) : 0;
        if (tile >= atlas_tiles * atlas_tiles) tile = 0;
        int tx = (tile % atlas_tiles) * tile_px;
        int ty = (tile / atlas_tiles) * tile_px;

        uint32_t rgba = ((uint32_t)DIR_SHADE[qd->dir] << 24) |
                        ((uint32_t)DIR_SHADE[qd->dir] << 16) |
                        ((uint32_t)DIR_SHADE[qd->dir] << 8) | 0xFF;

        /* 5,6,5 packed normal, straight off the face direction. */
        float nrm[3] = { 0, 0, 0 };
        nrm[an] = (float)sgn;
        uint16_t norm = t3d_vert_pack_normal(&(T3DVec3){{ nrm[0], nrm[1], nrm[2] }});

        for (int i = 0; i < 4; i++) {
            /* Negative-facing faces get the reversed corner order, or they are
             * wound inward and backface culling eats them. */
            int ci = (sgn > 0) ? i : (3 - i);
            int du = CORNER_U[ci] * qd->w;
            int dv = CORNER_V[ci] * qd->h;

            int pos[3];
            pos[au] = origin[au] + du;
            pos[av] = origin[av] + dv;
            pos[an] = plane;
            for (int a = 0; a < 3; a++)
                pos[a] = (int)(w->offset.v[a]) + pos[a] * B;

            /* UVs in s10.5 pixel coordinates. The tile repeats across a merged
             * quad rather than stretching: a 6-block wall must look like six
             * blocks, not one smeared texel, which is the whole reason greedy
             * merging needs a wrapping texparm at bind time. */
            int16_t s = (int16_t)((tx + du * tile_px) << 5);
            int16_t t = (int16_t)((ty + dv * tile_px) << 5);

            pack_vert(base, (int)(q * 4 + (uint32_t)i), pos, s, t, rgba, norm);
        }
    }

    out->verts = base;
    out->quad_count = quad_count;
    out->vert_count = quad_count * 4;

    /* The RSP DMAs these; a dirty cache line over them would be read as
     * geometry. malloc_uncached'd memory still needs the write to have landed. */
    data_cache_hit_writeback(base, need * sizeof(T3DVertPacked));
    return 0;
}

void kiln_voxmesh_draw(const KilnVoxMesh *m)
{
    if (!m->quad_count) return;

    uint32_t drawn = 0;
    while (drawn < m->quad_count) {
        uint32_t batch = m->quad_count - drawn;
        if (batch * 4 > KILN_VOXMESH_BATCH) batch = KILN_VOXMESH_BATCH / 4;

        /* One DMA for up to 17 quads. The offset is in vertices and the source
         * pointer walks in T3DVertPacked entries (two vertices each). */
        t3d_vert_load(m->verts + (drawn * 2), 0, batch * 4);
        for (uint32_t i = 0; i < batch; i++) {
            uint16_t b = (uint16_t)(i * 4);
            t3d_tri_draw(b, (uint16_t)(b + 1), (uint16_t)(b + 2));
            t3d_tri_draw(b, (uint16_t)(b + 2), (uint16_t)(b + 3));
        }
        t3d_tri_sync();
        drawn += batch;
    }
}

/* ── The runtime CI4 atlas ─────────────────────────────────────────────*/

/* A default ramp that separates by VALUE, not hue. Under a hue-discarding
 * palette-swap filter (see the n64-modeling skill) only value carries, so a
 * palette distinguished only by hue stops reading the moment such a filter
 * goes up — which makes this the correct default even before any such code
 * runs.
 * Index 0 is left fully transparent so a tile can have holes. */
static void default_palette(KilnVoxAtlas *a)
{
    a->tlut[KILN_VOXATLAS_COLD][0]   = 0x0000;   /* alpha 0 */
    a->tlut[KILN_VOXATLAS_VEILED][0] = 0x0000;

    /* The ramp starts at 6/31, not 0: a linear ramp from black puts entries 1
     * and 2 within two of each other out of 31, and at 16x16 on a CRT that is
     * one colour with two names. Index 0 is the transparent slot, so nothing
     * needs to be black — and "separate by VALUE" only helps if the values are
     * far enough apart to separate. */
    for (int i = 1; i < KILN_VOXATLAS_COLOURS; i++) {
        int v = 6 + (i * 25) / (KILN_VOXATLAS_COLOURS - 1);      /* 6..31 ramp */
        a->tlut[KILN_VOXATLAS_COLD][i] =
            (uint16_t)((v << 11) | (v << 6) | (v << 1) | 1);    /* grey */
        /* The veiled twin keeps the same VALUE and throws the rest at red, which
         * is the veil's whole grammar: same indices, different lookup. */
        int r = v, g = v / 4, b = v / 6;
        a->tlut[KILN_VOXATLAS_VEILED][i] =
            (uint16_t)((r << 11) | (g << 6) | (b << 1) | 1);
    }
}

/* A placeholder per tile that is distinguishable at 16x16 on a CRT: a flat
 * value with a one-texel border a shade darker. Flat tiles all look identical
 * once four of them are on screen, and "which block type am I holding" is the
 * question the editor is asked most often. */
static void default_tiles(KilnVoxAtlas *a)
{
    for (int tile = 0; tile < KILN_VOXATLAS_TILES; tile++) {
        /* body and edge four ramp steps apart, so the border is visible rather
         * than technically present: adjacent indices differ by under two units
         * of 31 and read as one flat tile. */
        uint8_t body = (uint8_t)(4 + (tile % 11));
        uint8_t edge = (uint8_t)(body >= 8 ? body - 4 : body + 4);
        for (int y = 0; y < KILN_VOXATLAS_TILE; y++)
            for (int x = 0; x < KILN_VOXATLAS_TILE; x++) {
                int border = (x == 0 || y == 0 ||
                              x == KILN_VOXATLAS_TILE - 1 ||
                              y == KILN_VOXATLAS_TILE - 1);
                kiln_voxatlas_plot(a, tile, x, y, border ? edge : body);
            }
    }
}

int kiln_voxatlas_init(KilnVoxAtlas *a)
{
    memset(a, 0, sizeof *a);

    /* 64x64 at 4bpp = 2 KB. Uncached because rdpq DMAs it to TMEM. */
    const int stride = KILN_VOXATLAS_SIDE / 2;
    a->packed = malloc_uncached(stride * KILN_VOXATLAS_SIDE);
    if (!a->packed) return -1;

    a->surface = surface_make_linear(a->packed, FMT_CI4,
                                     KILN_VOXATLAS_SIDE, KILN_VOXATLAS_SIDE);
    default_palette(a);
    default_tiles(a);
    a->dirty = 1;
    return 0;
}

void kiln_voxatlas_close(KilnVoxAtlas *a)
{
    if (a->packed) free_uncached(a->packed);
    a->packed = NULL;
}

static int atlas_xy(int tile, int px, int py, int *ox, int *oy)
{
    if (tile < 0 || tile >= KILN_VOXATLAS_TILES) return 0;
    if (px < 0 || py < 0 || px >= KILN_VOXATLAS_TILE || py >= KILN_VOXATLAS_TILE)
        return 0;
    *ox = (tile % KILN_VOXATLAS_ACROSS) * KILN_VOXATLAS_TILE + px;
    *oy = (tile / KILN_VOXATLAS_ACROSS) * KILN_VOXATLAS_TILE + py;
    return 1;
}

void kiln_voxatlas_plot(KilnVoxAtlas *a, int tile, int px, int py, uint8_t colour)
{
    int x, y;
    if (!atlas_xy(tile, px, py, &x, &y)) return;
    a->index[y * KILN_VOXATLAS_SIDE + x] = (uint8_t)(colour & 0x0F);
    a->dirty = 1;
}

uint8_t kiln_voxatlas_peek(const KilnVoxAtlas *a, int tile, int px, int py)
{
    int x, y;
    if (!atlas_xy(tile, px, py, &x, &y)) return 0;
    return a->index[y * KILN_VOXATLAS_SIDE + x];
}

void kiln_voxatlas_fill_tile(KilnVoxAtlas *a, int tile, uint8_t colour)
{
    for (int y = 0; y < KILN_VOXATLAS_TILE; y++)
        for (int x = 0; x < KILN_VOXATLAS_TILE; x++)
            kiln_voxatlas_plot(a, tile, x, y, colour);
}

void kiln_voxatlas_bind(KilnVoxAtlas *a, KilnVoxAtlasState state)
{
    if (!a->packed) return;

    if (a->dirty) {
        /* Pack the one-byte-per-pixel edit buffer down to 4bpp. High nibble is
         * the even (left) pixel, which is what FMT_CI4 expects. */
        uint8_t *dst = (uint8_t *)a->packed;
        for (int y = 0; y < KILN_VOXATLAS_SIDE; y++) {
            const uint8_t *src = &a->index[y * KILN_VOXATLAS_SIDE];
            for (int x = 0; x < KILN_VOXATLAS_SIDE; x += 2)
                *dst++ = (uint8_t)((src[x] << 4) | (src[x + 1] & 0x0F));
        }
        a->dirty = 0;
    }

    /* Repeat, not clamp: a greedy-merged quad's UVs run past one tile, and the
     * point is that a 6-block wall reads as six blocks. */
    rdpq_texparms_t parms = {
        .s.repeats = REPEAT_INFINITE,
        .t.repeats = REPEAT_INFINITE,
    };
    rdpq_tex_upload(TILE0, &a->surface, &parms);
    rdpq_tex_upload_tlut(a->tlut[state], 0, KILN_VOXATLAS_COLOURS);
    rdpq_mode_tlut(TLUT_RGBA16);
}
