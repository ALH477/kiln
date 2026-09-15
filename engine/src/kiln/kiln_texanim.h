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
 *   KILN_TEXANIM_FLIPBOOK N sprite frames, the current one uploaded as the
 *                        material's texture.
 *
 *   KILN_TEXANIM_PALETTE  A CI4/CI8 surface uploaded with one of N palettes,
 *                        chosen by time: palette cycling.
 *
 *   KILN_TEXANIM_OFFSCREEN A caller-provided surface_t uploaded as the texture.
 *                        The caller renders into it before kiln_texanim_draw
 *                        (before kiln_frame_begin, with rdpq_attach_clear).
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

    /** Not consulted: SCROLL applies to every textured material the model
     *  draws, and the reference modes match their material by `ref_id`. Kept
     *  so existing initialisers compile. */
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

    /** The material's texture reference number, 1..15: f3d_inject.py's
     *  refAddress. FLIPBOOK, PALETTE and OFFSCREEN draw only into the material
     *  that carries it; give each animated material its own. */
    uint8_t ref_id;

    /* ── Palette params (KILN_TEXANIM_PALETTE) ─────────────────────── */
    struct {
        surface_t *indices;   /**< caller-owned FMT_CI4 / FMT_CI8 surface */
        uint16_t **palettes;  /**< caller-owned RGBA16 palettes, 8-byte aligned */
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