/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_texanim.c — see m64_texanim.h for the model.
 */

#include "m64_texanim.h"

#include <string.h>

/* Context passed through t3d_model_draw_custom as userData. */
typedef struct {
    M64TexAnim *anims;
    int count;
} TexAnimCtx;

static M64TexAnim *find_anim(TexAnimCtx *ctx, const char *mat_name)
{
    if (!mat_name) return NULL;
    for (int i = 0; i < ctx->count; i++) {
        if (!ctx->anims[i].material_name) continue;
        if (strcmp(ctx->anims[i].material_name, mat_name) == 0)
            return &ctx->anims[i];
    }
    /* NULL material_name means "animate every material" — return first. */
    for (int i = 0; i < ctx->count; i++) {
        if (!ctx->anims[i].material_name)
            return &ctx->anims[i];
    }
    return NULL;
}

/* ── Tile callback: scroll + palette ──────────────────────────────────── */

static void tile_cb(void *userData, rdpq_texparms_t *tileParams,
                    rdpq_tile_t tile)
{
    /* The material name is not available in the tile callback signature;
     * we apply scroll/palette to all tiles. A multi-material model with
     * per-material scroll would need the filterCb or a name match via the
     * material pointer. For the common case (one animated material per
     * model), this is correct and simple. */
    TexAnimCtx *ctx = (TexAnimCtx *)userData;
    (void)tile;

    for (int i = 0; i < ctx->count; i++) {
        M64TexAnim *a = &ctx->anims[i];
        if (a->mode == M64_TEXANIM_SCROLL) {
            tileParams->s.translate = a->scroll.s_offset;
            tileParams->t.translate = a->scroll.t_offset;
        }
        if (a->mode == M64_TEXANIM_PALETTE) {
            int frame = (int)(a->palette.time * a->palette.fps) %
                        a->palette.pal_count;
            if (frame < 0) frame += a->palette.pal_count;
            rdpq_tex_upload_tlut(a->palette.palettes[frame], 0,
                                 a->palette.colors_per);
        }
    }
}

/* ── Dynamic texture callback: flipbook + offscreen ───────────────────── */

static void dyn_tex_cb(void *userData, const T3DMaterial *material,
                       rdpq_texparms_t *tileParams, rdpq_tile_t tile)
{
    (void)material;
    (void)tileParams;
    (void)tile;

    TexAnimCtx *ctx = (TexAnimCtx *)userData;
    for (int i = 0; i < ctx->count; i++) {
        M64TexAnim *a = &ctx->anims[i];
        if (a->mode == M64_TEXANIM_FLIPBOOK) {
            int frame = (int)(a->flipbook.time * a->flipbook.fps) %
                        a->flipbook.frame_count;
            if (frame < 0) frame += a->flipbook.frame_count;
            rdpq_set_lookup_address(a->ref_id,
                                    a->flipbook.frames[frame]->data);
        }
        if (a->mode == M64_TEXANIM_OFFSCREEN) {
            if (a->offscreen.surface) {
                rdpq_tex_upload(TILE0, a->offscreen.surface, NULL);
            }
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

void m64_texanim_update(M64TexAnim *anims, int count, float dt)
{
    for (int i = 0; i < count; i++) {
        M64TexAnim *a = &anims[i];
        switch (a->mode) {
        case M64_TEXANIM_SCROLL:
            a->scroll.s_offset += a->scroll.s_speed * dt;
            a->scroll.t_offset += a->scroll.t_speed * dt;
            break;
        case M64_TEXANIM_FLIPBOOK:
            a->flipbook.time += dt;
            break;
        case M64_TEXANIM_PALETTE:
            a->palette.time += dt;
            break;
        case M64_TEXANIM_OFFSCREEN:
            break;
        }
    }
}

void m64_texanim_draw(const T3DModel *model, M64TexAnim *anims, int count)
{
    if (!anims || count <= 0) {
        t3d_model_draw(model);
        return;
    }

    TexAnimCtx ctx = { .anims = anims, .count = count };

    int has_tile = 0, has_dyn = 0;
    for (int i = 0; i < count; i++) {
        if (anims[i].mode == M64_TEXANIM_SCROLL ||
            anims[i].mode == M64_TEXANIM_PALETTE)
            has_tile = 1;
        if (anims[i].mode == M64_TEXANIM_FLIPBOOK ||
            anims[i].mode == M64_TEXANIM_OFFSCREEN)
            has_dyn = 1;
    }

    t3d_model_draw_custom(model, (T3DModelDrawConf){
        .userData = &ctx,
        .tileCb = has_tile ? tile_cb : NULL,
        .dynTextureCb = has_dyn ? dyn_tex_cb : NULL,
    });
}