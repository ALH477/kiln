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
} TexAnimCtx;

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
        KilnTexAnim *a = &ctx->anims[i];
        if (a->mode == KILN_TEXANIM_SCROLL) {
            tileParams->s.translate = a->scroll.s_offset;
            tileParams->t.translate = a->scroll.t_offset;
        }
        if (a->mode == KILN_TEXANIM_PALETTE) {
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
    /* `material` IS available here, unlike in tile_cb, and is deliberately
     * unused: every registered animation is applied to every material, which
     * is right for the one-animated-material-per-model case every consumer in
     * this repo has. A per-material path would match material->name against
     * KilnTexAnim.material_name — a `find_anim` doing exactly that used to sit
     * above, unreferenced, and native -Werror is what pointed it out. If that
     * path is ever wanted, write it here where the pointer is, rather than
     * above where it was not reachable. */
    (void)material;
    (void)tileParams;
    (void)tile;

    TexAnimCtx *ctx = (TexAnimCtx *)userData;
    for (int i = 0; i < ctx->count; i++) {
        KilnTexAnim *a = &ctx->anims[i];
        if (a->mode == KILN_TEXANIM_FLIPBOOK) {
            int frame = (int)(a->flipbook.time * a->flipbook.fps) %
                        a->flipbook.frame_count;
            if (frame < 0) frame += a->flipbook.frame_count;
            rdpq_set_lookup_address(a->ref_id,
                                    a->flipbook.frames[frame]->data);
        }
        if (a->mode == KILN_TEXANIM_OFFSCREEN) {
            if (a->offscreen.surface) {
                rdpq_tex_upload(TILE0, a->offscreen.surface, NULL);
            }
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
        if (anims[i].mode == KILN_TEXANIM_SCROLL ||
            anims[i].mode == KILN_TEXANIM_PALETTE)
            has_tile = 1;
        if (anims[i].mode == KILN_TEXANIM_FLIPBOOK ||
            anims[i].mode == KILN_TEXANIM_OFFSCREEN)
            has_dyn = 1;
    }

    t3d_model_draw_custom(model, (T3DModelDrawConf){
        .userData = &ctx,
        .tileCb = has_tile ? tile_cb : NULL,
        .dynTextureCb = has_dyn ? dyn_tex_cb : NULL,
    });
}