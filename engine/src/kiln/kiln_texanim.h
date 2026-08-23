/* SPDX-License-Identifier: MIT
 *
 * kiln_texanim.h — texture animation for .t3dm models.
 *
 * Four modes, all built on Tiny3D's draw-custom callbacks:
 *
 *   KILN_TEXANIM_SCROLL   Per-material UV offset via rdpq tile translate.
 *                        The tile callback modifies rdpq_texparms_t each frame.
 *                        Works with any textured .t3dm; no special authoring.
 *
 *   KILN_TEXANIM_FLIPBOOK N sprite frames swapped via rdpq_set_lookup_address.
 *                        The model's material must be authored with
 *                        use_tex_reference (f3d_inject.py's useRef=1). The
 *                        callback sets the lookup address before the DPL runs.
 *
 *   KILN_TEXANIM_PALETTE  CI4/CI8 palette cycling via rdpq_tex_upload_tlut.
 *                        The tile callback uploads a new palette per frame.
 *
 *   KILN_TEXANIM_OFFSCREEN A caller-provided surface_t is uploaded into a
 *                        material's texReference slot each frame. The caller
 *                        renders into the surface before calling kiln_texanim_draw.
 *
 * ── Why a tile callback, not a recorded DPL ─────────────────────────────
 * Scrolling and palette modes must change tile params per frame, which is
 * impossible with a pre-recorded display list (params are baked into the DPL).
 * Flipbook and offscreen modes CAN use a recorded DPL (the lookup address or
 * surface pointer changes before rspq_block_run), but for API uniformity all
 * four modes go through t3d_model_draw_custom. The cost is negligible — the
 * custom draw path is what t3d_model_draw itself calls internally.
 *
 * ── No state leaks ──────────────────────────────────────────────────────
 * kiln_texanim_draw saves the current combiner before the custom draw and
 * restores it after, so a textured animated model does not leave the RDP in
 * TEX_SHADE mode for the next actor that expects SHADE (the scene's default).
 */
#ifndef KILN_TEXANIM_H
#define KILN_TEXANIM_H

#include <t3d/t3dmodel.h>
#include <libdragon.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KILN_TEXANIM_SCROLL   = 0,
    KILN_TEXANIM_FLIPBOOK = 1,
    KILN_TEXANIM_PALETTE  = 2,
    KILN_TEXANIM_OFFSCREEN = 3,
} KilnTexAnimMode;

typedef struct {
    KilnTexAnimMode mode;

    /** Which material in the model to animate, by name. The model may have
     *  other materials that are not animated; only the named one gets the
     *  callback. NULL animates every material. */
    const char *material_name;

    /* ── Scroll params (KILN_TEXANIM_SCROLL) ─────────────────────────── */
    struct {
        float s_speed;    /**< pixels per second */
        float t_speed;
        float s_offset;   /**< current offset (advanced by update) */
        float t_offset;
    } scroll;

    /* ── Flipbook params (KILN_TEXANIM_FLIPBOOK) ────────────────────── */
    struct {
        sprite_t **frames;  /**< caller-owned array of loaded sprites */
        int frame_count;
        float fps;          /**< frames per second */
        float time;         /**< accumulated time */
    } flipbook;

    /** The lookup index matching the model's tex_reference field. Set by
     *  the build (f3d_inject.py refAddress). Used by flipbook + offscreen. */
    uint32_t ref_id;

    /* ── Palette params (KILN_TEXANIM_PALETTE) ─────────────────────── */
    struct {
        uint16_t **palettes;  /**< caller-owned array of palette data */
        int pal_count;
        int colors_per;        /**< entries per palette (16 for CI4, 256 for CI8) */
        float fps;
        float time;
    } palette;

    /* ── Offscreen params (KILN_TEXANIM_OFFSCREEN) ─────────────────── */
    struct {
        surface_t *surface;  /**< caller sets this each frame before draw */
    } offscreen;
} KilnTexAnim;

/** Advance all animation timers. Call once per frame before kiln_texanim_draw.
 *  `dt` is seconds. */
void kiln_texanim_update(KilnTexAnim *anims, int count, float dt);

/** Draw a model with texture animation. Calls t3d_model_draw_custom with the
 *  appropriate callbacks. Must be called inside the 3D pass (between
 *  kiln_scene_begin and kiln_gui_begin) after the model's transform is pushed.
 *  `anims` may be NULL (draws without animation, equivalent to t3d_model_draw). */
void kiln_texanim_draw(const T3DModel *model, KilnTexAnim *anims, int count);

#ifdef __cplusplus
}
#endif

#endif /* KILN_TEXANIM_H */