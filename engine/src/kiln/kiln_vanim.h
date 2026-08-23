/* SPDX-License-Identifier: MIT
 *
 * kiln_vanim.h — procedural vertex animation and morph target blending.
 *
 * Three capabilities, all built on Tiny3D's vertex buffer access +
 * segment-based buffer swapping (see Tiny3D examples/04_dynamic):
 *
 *   kiln_morph_*   Blend between N vertex buffers (morph targets) into a
 *                 working buffer via CPU lerp. Each target is a sibling
 *                 .t3dm with identical topology.
 *
 *   kiln_deform_*  A user callback modifies vertex positions/normals/colours
 *                 per frame (water waves, wind sway, flag ripple).
 *
 *   kiln_vfx_*     Thin wrapper around t3d_state_set_vertex_fx for RSP-side
 *                 effects: spherical UV (env mapping), cel-shading, outline,
 *                 global UV offset. Zero CPU cost.
 *
 * ── Why CPU-side, not RSP ───────────────────────────────────────────────
 * Tiny3D's RSP ucode is fixed — no programmable vertex shaders, no morph
 * target support, no per-vertex blend. gltf_to_t3d does not parse glTF
 * morph targets. The only way to do non-rigid vertex animation on this
 * hardware is to modify the T3DVertPacked buffer on the CPU before
 * t3d_vert_load DMAs it to the RSP. This module packages that pattern with
 * multi-buffering (avoids RSP/CPU races) and segment-based addressing
 * (works with the model's recorded object draw commands).
 *
 * ── Multi-buffering ─────────────────────────────────────────────────────
 * The RSP may still be reading last frame's vertex buffer when the CPU
 * starts writing this frame's. Two buffers cycled per frame are sufficient
 * (the RSP finishes within one frame). Three buffers are for safety under
 * heavy load. The caller picks; 2 is the default.
 *
 * ── No libm in the hot path ─────────────────────────────────────────────
 * Morph blending uses fm_vec3_lerp (one mul + add per axis). Procedural
 * deform callbacks may use fm_sinf sparingly (the interceptor-demo's streaks
 * do), but the engine's stance is: no gratuitous libm. A water surface that
 * needs a sine per vertex should tabulate it at init.
 */
#ifndef KILN_VANIM_H
#define KILN_VANIM_H

#include <stdint.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Vertex FX (RSP-side, zero CPU cost) ─────────────────────────────── */

typedef enum {
    KILN_VFX_NONE           = 0,
    KILN_VFX_SPHERICAL_UV   = 1,  /**< env mapping; arg0=w, arg1=h */
    KILN_VFX_CELSHADE_COLOR = 2,
    KILN_VFX_CELSHADE_ALPHA = 3,
    KILN_VFX_OUTLINE        = 4,  /**< arg0=pixel_w, arg1=pixel_h */
    KILN_VFX_UV_OFFSET     = 5,  /**< arg0/arg1 = UV offset (10.5 fixed) */
} KilnVertexFX;

/** Set a global RSP vertex effect. Applies to all subsequent t3d_vert_load
 *  calls until kiln_vfx_clear. Call between kiln_scene_begin and the draw. */
void kiln_vfx_set(KilnVertexFX fx, int16_t arg0, int16_t arg1);

/** Disable vertex FX (equivalent to kiln_vfx_set(KILN_VFX_NONE, 0, 0)). */
void kiln_vfx_clear(void);

/* ── Morph target blending ──────────────────────────────────────────── */

/** A set of morph targets blended into a working buffer each frame. */
typedef struct {
    const T3DModel *model;       /**< borrowed; provides mesh topology */
    T3DVertPacked **targets;     /**< N source vertex buffers (uncached) */
    int target_count;
    float *weights;              /**< per-target weights, summed and normalised */
    T3DVertPacked *work_buffers;  /**< uncached, buffer_count copies */
    int buffer_count;            /**< 2 or 3 */
    int current_buffer;           /**< cycles 0..buffer_count-1 */
    uint8_t segment_id;           /**< segment 1-6 for placeholder addressing */
    bool initialised;             /**< t3d_model_make_object_vert_placeholder done */
} KilnMorph;

/** Initialise the morph set. `targets` must contain `target_count` vertex
 *  buffers obtained from sibling .t3dm models with the same vertex count.
 *  `buffer_count` is 2 (default) or 3. `segment_id` is 1-6 (use a different
 *  one per concurrent morphed model). Allocates uncached work buffers. */
void kiln_morph_init(KilnMorph *m, const T3DModel *model,
                    T3DVertPacked **targets, int target_count,
                    int buffer_count, uint8_t segment_id);

/** Free work buffers. Does not free `model` or `targets` (caller-owned). */
void kiln_morph_destroy(KilnMorph *m);

/** Blend `targets` by `weights` into the current work buffer, then advance
 *  the buffer index. Weights are clamped to [0,1] and normalised so they
 *  sum to 1. Call once per frame before kiln_morph_draw. */
void kiln_morph_update(KilnMorph *m, float dt);

/** Set the segment to the current work buffer, then draw the model. Call
 *  inside the 3D pass after kiln_transform_push. */
void kiln_morph_draw(KilnMorph *m);

/* ── Procedural deformation ──────────────────────────────────────────── */

/** Callback that modifies a vertex buffer in place. `time` is the
 *  accumulated animation time; `user_data` is opaque. */
typedef void (*KilnDeformFn)(T3DVertPacked *verts, int vert_count,
                            float time, void *user_data);

typedef struct {
    const T3DModel *model;
    KilnDeformFn fn;
    void *user_data;
    T3DVertPacked *work_buffers;
    T3DVertPacked *base_buffer;   /**< copy of original vertices (for reset) */
    int vert_count;
    int buffer_count;
    int current_buffer;
    uint8_t segment_id;
    float time;
    bool initialised;
} KilnDeform;

/** Initialise from a model. Allocates uncached work buffers + a base copy.
 *  The base copy preserves the original vertices so the deform callback can
 *  work from a known reference each frame. */
void kiln_deform_init(KilnDeform *d, const T3DModel *model,
                     KilnDeformFn fn, void *user_data,
                     int buffer_count, uint8_t segment_id);

void kiln_deform_destroy(KilnDeform *d);

/** Copy base into current work buffer, call the deform function, advance
 *  the buffer index. Call once per frame before kiln_deform_draw. */
void kiln_deform_update(KilnDeform *d, float dt);

/** Set the segment + draw. Call inside the 3D pass after push. */
void kiln_deform_draw(KilnDeform *d);

#ifdef __cplusplus
}
#endif

#endif /* KILN_VANIM_H */