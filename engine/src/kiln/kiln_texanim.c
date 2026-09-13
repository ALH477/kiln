/* SPDX-License-Identifier: MIT
 *
 * kiln_texanim.c — see kiln_texanim.h for the model.
 */

#include "kiln_texanim.h"

#include <string.h>

/* Context passed through t3d_model_draw_custom as userData. */
typedef struct {
    KilnTexAnim *anims;
    int count;
    int tlut;    /* a PALETTE upload turned TLUT sampling on */
} TexAnimCtx;

/* ── Tile callback: scroll ───────────────────────────────────────────── */

static void tile_cb(void *userData, rdpq_texparms_t *tileParams,
                    rdpq_tile_t tile)
{
    /* No material here, so a scroll applies to every textured material the
     * model draws — right for the one-textured-material models that use it. */
    TexAnimCtx *ctx = (TexAnimCtx *)userData;
    (void)tile;

    for (int i = 0; i < ctx->count; i++) {
        const KilnTexAnim *a = &ctx->anims[i];
        if (a->mode == KILN_TEXANIM_SCROLL) {
            tileParams->s.translate = a->scroll.s_offset;
            tileParams->t.translate = a->scroll.t_offset;
        }
    }
}

/* ── Dynamic texture callback: flipbook, palette, offscreen ───────────── */

static int frame_of(float time, float fps, int count)
{
    int f = (int)(time * fps) % count;
    return f < 0 ? f + count : f;
}

static void dyn_tex_cb(void *userData, const T3DMaterial *material,
                       rdpq_texparms_t *tileParams, rdpq_tile_t tile)
{
    /* Called INSTEAD of Tiny3D's upload for a texture-reference material, so
     * whatever matches uploads here or the material draws untextured. */
    TexAnimCtx *ctx = (TexAnimCtx *)userData;
    const uint32_t ref = material->textureA.texReference;

    for (int i = 0; i < ctx->count; i++) {
        KilnTexAnim *a = &ctx->anims[i];
        if (a->mode == KILN_TEXANIM_SCROLL || a->ref_id != ref) continue;

        switch (a->mode) {
        case KILN_TEXANIM_FLIPBOOK: {
            const int f = frame_of(a->flipbook.time, a->flipbook.fps, a->flipbook.frame_count);
            surface_t s = sprite_get_pixels(a->flipbook.frames[f]);
            rdpq_tex_upload(tile, &s, tileParams);
            break;
        }
        case KILN_TEXANIM_PALETTE: {
            const int f = frame_of(a->palette.time, a->palette.fps, a->palette.pal_count);
            rdpq_tex_upload(tile, a->palette.indices, tileParams);
            rdpq_tex_upload_tlut(a->palette.palettes[f], 0, a->palette.colors_per);
            rdpq_mode_tlut(TLUT_RGBA16);
            ctx->tlut = 1;
            break;
        }
        case KILN_TEXANIM_OFFSCREEN:
            if (a->offscreen.surface) rdpq_tex_upload(tile, a->offscreen.surface, tileParams);
            break;
        default:
            break;
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

void kiln_texanim_update(KilnTexAnim *anims, int count, float dt)
{
    for (int i = 0; i < count; i++) {
        KilnTexAnim *a = &anims[i];
        switch (a->mode) {
        case KILN_TEXANIM_SCROLL:
            a->scroll.s_offset += a->scroll.s_speed * dt;
            a->scroll.t_offset += a->scroll.t_speed * dt;
            break;
        case KILN_TEXANIM_FLIPBOOK:
            a->flipbook.time += dt;
            break;
        case KILN_TEXANIM_PALETTE:
            a->palette.time += dt;
            break;
        case KILN_TEXANIM_OFFSCREEN:
            break;
        }
    }
}

void kiln_texanim_draw(const T3DModel *model, KilnTexAnim *anims, int count)
{
    if (!anims || count <= 0) {
        t3d_model_draw(model);
        return;
    }

    TexAnimCtx ctx = { .anims = anims, .count = count };

    int has_tile = 0, has_dyn = 0;
    for (int i = 0; i < count; i++) {
        if (anims[i].mode == KILN_TEXANIM_SCROLL) {
            has_tile = 1;
        } else {
            /* rdpq's lookup slots, and so f3d's reference numbers, are 1..15;
             * 0 means "no texture" to Tiny3D. */
            assertf(anims[i].ref_id >= 1 && anims[i].ref_id <= 15,
                    "kiln_texanim: ref_id %d is not a texture reference (1..15)", anims[i].ref_id);
            has_dyn = 1;
        }
    }

    t3d_model_draw_custom(model, (T3DModelDrawConf){
        .userData = &ctx,
        .tileCb = has_tile ? tile_cb : NULL,
        .dynTextureCb = has_dyn ? dyn_tex_cb : NULL,
    });

    if (ctx.tlut) rdpq_mode_tlut(TLUT_NONE);
}
