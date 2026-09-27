/* SPDX-License-Identifier: MIT
 *
 * kiln_detail.h — draw a model's objects selectively: by visibility, and by
 * level of detail.
 *
 * This is the Tiny3D-facing half of per-object culling. The arithmetic lives in
 * kiln_cull.h, which is pure and asserted on the host; this file is the part
 * that touches a T3DModel and therefore cannot be (see
 * nix/checks/kiln-logic.nix's header on why that split exists).
 *
 * ── What it replaces ───────────────────────────────────────────────────
 * `t3d_model_draw(model)` submits EVERY object in the model, every frame,
 * whatever the camera is looking at. That is the right default and it is also
 * the whole cost: a scene knows things the model does not — where it is, how
 * far away, whether it is behind the eye.
 *
 * fig_detail_draw is that same loop with two questions asked first.
 *
 * ── 1. Is it in frame? ─────────────────────────────────────────────────
 * Tiny3D already gives every object its own bounding box (T3DObject.aabbMin /
 * aabbMax, present in every model whether or not it was built with `--bvh`)
 * and ships the plane test (t3d_frustum_vs_sphere). Nothing was asking.
 *
 * ── 2. Which detail tier? ──────────────────────────────────────────────
 * A model may carry lower-detail versions of its objects as EXTRA OBJECTS in
 * the same .t3dm, named with a `.lodN` suffix:
 *
 *     gargoyle_shell         492 tris    tier 0
 *     gargoyle_shell.lod1    ~180 tris   tier 1
 *     gargoyle_shell.lod2     ~60 tris   tier 2
 *
 * One .t3dm, one load, one residency, and the tiers share their material — so
 * a tier costs no new f3d_inject spec, no new texture and no new fog decision.
 * That falls out of how the importer works: it makes one T3DObject per glTF
 * PRIMITIVE and names it from the NODE (gltf_importer parser.cpp:181-183),
 * while materials live in a separate table keyed by name.
 *
 * ── The authoring rule, which is a rule because the cheap version needs it ──
 * At a requested tier N, this draws the objects whose own tier is exactly the
 * highest tier present in the model that is <= N. It does NOT try to pair each
 * object with its own base name's best variant, because that would mean
 * grouping objects by base name — string work, per object, per frame, on a
 * 93.75 MHz VR4300.
 *
 * So a tier must be COMPLETE: if a model has any `.lod1` objects, then
 * everything meant to be visible at tier 1 needs a `.lod1`, including the small
 * parts. An object left without one simply is not drawn at that tier — which is
 * usable on purpose (drop a four-triangle eye highlight at distance by giving it
 * no `.lod2`), and is a hole in the model if it was an oversight. The
 * per-tier triangle counts printed by every model build are what show the
 * difference.
 *
 * A model with no `.lodN` objects at all is unaffected: every tier request
 * resolves to tier 0, which is every object it has.
 */
#ifndef FIG_DETAIL_H
#define FIG_DETAIL_H


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

#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <t3d/t3dmodel.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where an instance is, for the frustum test. Uniform scale only — pass the
 *  largest axis of a non-uniform one, which keeps the bound conservative.
 *  `yaw` is radians about +Y in the engine's convention (yaw 0 faces +Z). */
typedef struct {
    float pos[3];
    float scale;
    float yaw;
} FigDetailPlace;

/** Per-frame accounting. This exists because the cost being managed here is
 *  invisible: a frame that culls half its objects and a frame that culls none
 *  look identical, and the only difference is how long they took. */
typedef struct {
    uint16_t drawn;        /**< objects submitted */
    uint16_t culled;       /**< objects rejected by the frustum */
    uint16_t skipped;      /**< objects of a tier that was not asked for */
    uint16_t tris;         /**< triangles submitted */
    uint16_t verts;        /**< vertices submitted */
    uint16_t parts;        /**< RSP vertex loads submitted */
    uint16_t tier_drawn[4];/**< objects submitted per tier, 0..3 */
    uint16_t models_culled;/**< whole instances rejected before any object */
    uint16_t models_drawn; /**< whole instances that passed that test */
} FigDetailStats;

/** Is any of this model in frame?
 *
 *  Tested against the whole-model bounding box (T3DModel.aabbMin/aabbMax,
 *  present in every model), so it is one sphere test for the entire instance.
 *  Worth asking FIRST and separately from the per-object loop, because a
 *  caller that gets `false` can skip the matrix upload as well as the
 *  geometry — Tiny3D's own culling example opens the same way.
 *
 *  Counts a model-level rejection in the frame stats.
 */
int fig_detail_model_visible(const T3DModel *model, const T3DFrustum *frustum,
                              const FigDetailPlace *place);

/** Zero the counters. Call once per frame, before any drawing. */
void fig_detail_frame_begin(void);

/** The counters for the frame so far. Never NULL. */
const FigDetailStats *fig_detail_stats(void);

/** The highest tier <= `want` that this model actually has objects for.
 *  A model with no tiers answers 0 for every request. */
uint8_t fig_detail_tier_available(const T3DModel *model, uint8_t want);

/** Should this object be drawn, at this tier, from this place?
 *
 *  Records the verdict in the frame counters, so a caller with its own draw
 *  loop — pm_veil_draw_model in PetaByte Madness is one — gets the culling and
 *  the accounting by adding a single `continue`.
 *
 *  `tier` is an ALREADY-RESOLVED tier, i.e. what fig_detail_tier_available
 *  returned; passing a raw request would silently drop everything in a model
 *  whose tiers stop short of it. `frustum` NULL skips the visibility test,
 *  which is what a model drawn without a known placement wants.
 */
int fig_detail_admit(const T3DObject *obj, uint8_t tier,
                      const T3DFrustum *frustum,
                      const FigDetailPlace *place);

/** The object to draw for `base` at `tier`.
 *
 *  Returns `base.lodN` for the highest N <= tier the model actually has, and
 *  `base` itself otherwise — so a base name with no tiers answers the same
 *  object whatever tier is asked for.
 *
 *  This is the atlas case: a model drawn one named object at a time
 *  (t3d_model_get_object) never reaches fig_detail_draw, which is where tiers
 *  are otherwise resolved. PetaByte Madness' palms model is sixteen separate
 *  props in one .t3dm, and every one of them is placed by name.
 *
 *  NULL if neither the tier nor the base resolves.
 */
T3DObject *fig_detail_object(const T3DModel *model, const char *base,
                              uint8_t tier);

/** Count an object the caller is drawing itself, having already decided to.
 *  For draw paths that bypass fig_detail_admit — t3d_model_draw_object on a
 *  named object, say — so the gauge still adds up. */
void fig_detail_count(const T3DObject *obj, uint8_t tier);

/** t3d_model_draw with the two questions asked. `bones` as for
 *  t3d_model_draw_object: NULL for an unskinned model. */
void fig_detail_draw(const T3DModel *model, const T3DMat4FP *bones,
                      uint8_t tier, const T3DFrustum *frustum,
                      const FigDetailPlace *place);

#ifdef __cplusplus
}
#endif

#endif /* FIG_DETAIL_H */
