/* SPDX-License-Identifier: MIT
 *
 * kiln_texanim.h — texture animation for .t3dm models.
 *
 * Four modes, all built on Tiny3D's draw-custom callbacks:
 *
 *   FIG_TEXANIM_SCROLL   Per-material UV offset via rdpq tile translate.
 *                        The tile callback modifies rdpq_texparms_t each frame.
 *                        Works with any textured .t3dm; no special authoring.
 *
 *   FIG_TEXANIM_FLIPBOOK N sprite frames, the current one uploaded as the
 *                        material's texture.
 *
 *   FIG_TEXANIM_PALETTE  A CI4/CI8 surface uploaded with one of N palettes,
 *                        chosen by time: palette cycling.
 *
 *   FIG_TEXANIM_OFFSCREEN A caller-provided surface_t uploaded as the texture.
 *                        The caller renders into it before fig_texanim_draw
 *                        (before fig_frame_begin, with rdpq_attach_clear).
 *
 * The last three need a material authored as a texture REFERENCE
 * (f3d_inject.py's useRef=1,refAddress=N,refSize=W:H), and match it by that
 * number, `ref_id`. Tiny3D uploads nothing for such a material — its
 * dynTextureCb is called INSTEAD of the upload — so the callback here does the
 * upload. Two things are wrong with the obvious alternatives, and this module
 * used to do both:
 *   * rdpq_set_lookup_address only binds a placeholder inside a RECORDED
 *     block. Tiny3D's draw is immediate, so a flipbook built on it drew the
 *     material with no texture at all.
 *   * A palette uploaded from the tile callback is overwritten: that callback
 *     runs BEFORE the material's own sprite upload, and rdpq_sprite_upload
 *     uploads the sprite's embedded palette. So a palette exhibit is a
 *     reference material with a CI surface the caller owns.
 * refSize must be the runtime surface's size: gltf_to_t3d bakes the UVs
 * against it.
 *
 * ── Why a tile callback, not a recorded DPL ─────────────────────────────
 * Scrolling and palette modes must change tile params per frame, which is
 * impossible with a pre-recorded display list (params are baked into the DPL).
 * Flipbook and offscreen modes CAN use a recorded DPL (the lookup address or
 * surface pointer changes before rspq_block_run), but for API uniformity all
 * four modes go through t3d_model_draw_custom. The cost is negligible — the
 * custom draw path is what t3d_model_draw itself calls internally.
 *
 * ── State left behind ────────────────────────────────────────────────────
 * Like t3d_model_draw, the draw leaves the model's material combiner set; the
 * next thing drawn sets its own. The one mode this module turns on itself —
 * TLUT sampling, for PALETTE — is turned back off after the draw, because an
 * RGBA texture sampled through a TLUT is garbage and nothing else here would
 * reset it.
 */
#ifndef FIG_TEXANIM_H
#define FIG_TEXANIM_H


/* The prefix migration train (docs/NAMING.md section 9 step 2). Pulled in by
 * every public header (a quoted include, so it resolves both in this tree and
 * in the installed include/kiln prefix) rather than force-included by
 * kiln-inst.mk, because a
 * force-include only reaches builds that include that file — a Nix check or a
 * host build compiling a downstream's sources directly never saw it, and
 * PetaByte-Madness' pm-cine check is what proved that. Deleting the train is
 * still a scripted one-line removal from these headers plus the file itself.
 */
#include "kiln_compat.h"

#include <t3d/t3dmodel.h>
#include <libdragon.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FIG_TEXANIM_SCROLL   = 0,
    FIG_TEXANIM_FLIPBOOK = 1,
    FIG_TEXANIM_PALETTE  = 2,
    FIG_TEXANIM_OFFSCREEN = 3,
} FigTexAnimMode;

typedef struct {
    FigTexAnimMode mode;

    /** Not consulted: SCROLL applies to every textured material the model
     *  draws, and the reference modes match their material by `ref_id`. Kept
     *  so existing initialisers compile. */
    const char *material_name;

    /* ── Scroll params (FIG_TEXANIM_SCROLL) ─────────────────────────── */
    struct {
        float s_speed;    /**< pixels per second */
        float t_speed;
        float s_offset;   /**< current offset (advanced by update) */
        float t_offset;
    } scroll;

    /* ── Flipbook params (FIG_TEXANIM_FLIPBOOK) ────────────────────── */
    struct {
        sprite_t **frames;  /**< caller-owned array of loaded sprites */
        int frame_count;
        float fps;          /**< frames per second */
        float time;         /**< accumulated time */
    } flipbook;

    /** The material's texture reference number, 1..15: f3d_inject.py's
     *  refAddress. FLIPBOOK, PALETTE and OFFSCREEN draw only into the material
     *  that carries it; give each animated material its own. */
    uint8_t ref_id;

    /* ── Palette params (FIG_TEXANIM_PALETTE) ─────────────────────── */
    struct {
        surface_t *indices;   /**< caller-owned FMT_CI4 / FMT_CI8 surface */
        uint16_t **palettes;  /**< caller-owned RGBA16 palettes, 8-byte aligned */
        int pal_count;
        int colors_per;        /**< entries per palette (16 for CI4, 256 for CI8) */
        float fps;
        float time;
    } palette;

    /* ── Offscreen params (FIG_TEXANIM_OFFSCREEN) ─────────────────── */
    struct {
        surface_t *surface;  /**< caller sets this each frame before draw */
    } offscreen;
} FigTexAnim;

/** Advance all animation timers. Call once per frame before fig_texanim_draw.
 *  `dt` is seconds. */
void fig_texanim_update(FigTexAnim *anims, int count, float dt);

/** Draw a model with texture animation. Calls t3d_model_draw_custom with the
 *  appropriate callbacks. Must be called inside the 3D pass (between
 *  fig_scene_begin and fig_gui_begin) after the model's transform is pushed.
 *  `anims` may be NULL (draws without animation, equivalent to t3d_model_draw). */
void fig_texanim_draw(const T3DModel *model, FigTexAnim *anims, int count);

#ifdef __cplusplus
}
#endif

#endif /* FIG_TEXANIM_H */