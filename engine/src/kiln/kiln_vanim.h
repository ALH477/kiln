/* SPDX-License-Identifier: MIT
 *
 * kiln_vanim.h — procedural vertex animation and morph target blending.
 *
 * Three capabilities, all built on Tiny3D's vertex buffer access +
 * segment-based buffer swapping (see Tiny3D examples/04_dynamic):
 *
 *   fig_morph_*   Blend between N vertex buffers (morph targets) into a
 *                 working buffer via CPU lerp. Each target is a sibling
 *                 .t3dm with identical topology.
 *
 *   fig_deform_*  A user callback modifies vertex positions/normals/colours
 *                 per frame (water waves, wind sway, flag ripple).
 *
 *   fig_vfx_*     Thin wrapper around t3d_state_set_vertex_fx for RSP-side
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
 * deform callbacks may use fm_sinf sparingly (texanim-demo's flag does, per
 * vertex), but the engine's stance is: no gratuitous libm. A water surface that
 * needs a sine per vertex should tabulate it at init.
 */
#ifndef FIG_VANIM_H
#define FIG_VANIM_H


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

#include <stdint.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Vertex FX (RSP-side, zero CPU cost) ─────────────────────────────── */

typedef enum {
    FIG_VFX_NONE           = 0,
    FIG_VFX_SPHERICAL_UV   = 1,  /**< env mapping; arg0=w, arg1=h */
    FIG_VFX_CELSHADE_COLOR = 2,
    FIG_VFX_CELSHADE_ALPHA = 3,
    FIG_VFX_OUTLINE        = 4,  /**< arg0=pixel_w, arg1=pixel_h */
    FIG_VFX_UV_OFFSET     = 5,  /**< arg0/arg1 = UV offset (10.5 fixed) */
} FigVertexFX;

/** Set a global RSP vertex effect. Applies to all subsequent t3d_vert_load
 *  calls until fig_vfx_clear. Call between fig_scene_begin and the draw. */
void fig_vfx_set(FigVertexFX fx, int16_t arg0, int16_t arg1);

/** Disable vertex FX (equivalent to fig_vfx_set(FIG_VFX_NONE, 0, 0)). */
void fig_vfx_clear(void);

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
} FigMorph;

/** Initialise the morph set. `targets` must contain `target_count` vertex
 *  buffers obtained from sibling .t3dm models with the same vertex count.
 *  `buffer_count` is 2 (default) or 3. `segment_id` is 1-6 (use a different
 *  one per concurrent morphed model). Allocates uncached work buffers. */
void fig_morph_init(FigMorph *m, const T3DModel *model,
                    T3DVertPacked **targets, int target_count,
                    int buffer_count, uint8_t segment_id);

/** Free work buffers. Does not free `model` or `targets` (caller-owned). */
void fig_morph_destroy(FigMorph *m);

/** Blend `targets` by `weights` into the current work buffer, then advance
 *  the buffer index. Weights are clamped to [0,1] and normalised so they
 *  sum to 1. Call once per frame before fig_morph_draw. */
void fig_morph_update(FigMorph *m, float dt);

/** Set the segment to the current work buffer, then draw the model. Call
 *  inside the 3D pass after fig_transform_push. */
void fig_morph_draw(FigMorph *m);

/* ── Procedural deformation ──────────────────────────────────────────── */

/** Callback that modifies a vertex buffer in place. `time` is the
 *  accumulated animation time; `user_data` is opaque. */
typedef void (*FigDeformFn)(T3DVertPacked *verts, int vert_count,
                            float time, void *user_data);

typedef struct {
    const T3DModel *model;
    FigDeformFn fn;
    void *user_data;
    T3DVertPacked *work_buffers;
    T3DVertPacked *base_buffer;   /**< copy of original vertices (for reset) */
    int vert_count;
    int buffer_count;
    int current_buffer;
    uint8_t segment_id;
    float time;
    bool initialised;
} FigDeform;

/** Initialise from a model. Allocates uncached work buffers + a base copy.
 *  The base copy preserves the original vertices so the deform callback can
 *  work from a known reference each frame. */
void fig_deform_init(FigDeform *d, const T3DModel *model,
                     FigDeformFn fn, void *user_data,
                     int buffer_count, uint8_t segment_id);

void fig_deform_destroy(FigDeform *d);

/** Copy base into current work buffer, call the deform function, advance
 *  the buffer index. Call once per frame before fig_deform_draw. */
void fig_deform_update(FigDeform *d, float dt);

/** Set the segment + draw. Call inside the 3D pass after push. */
void fig_deform_draw(FigDeform *d);

#ifdef __cplusplus
}
#endif

#endif /* FIG_VANIM_H */