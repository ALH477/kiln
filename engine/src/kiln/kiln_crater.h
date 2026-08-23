/* SPDX-License-Identifier: MIT
 *
 * kiln_crater.h — destruction and reconstruction for static terrain, by
 * direct vertex mutation of a model's own resident buffer.
 *
 * ── A narrower sibling of kiln_vanim's kiln_deform_*, not a reuse of it ────
 * kiln_deform_* is built for continuous, whole-model deformation running
 * every frame forever (wind, waves): it copies the model's ENTIRE vertex
 * buffer into 2-3 uncached work buffers and redirects the model's draw
 * through a placeholder segment, so the RSP is never reading a buffer the
 * CPU is still writing. That is the right cost for something that never
 * stops changing.
 *
 * A crater is the opposite shape: it changes rarely, briefly, and only
 * within a handful of vertices of ONE named object inside a model that may
 * have several (a terrain object sharing a .t3dm with gate and landmark
 * objects, say). Paying for a whole-model segment redirect and
 * multi-buffering for that is the wrong cost. This module instead
 * generalises a sea-swell pattern a downstream game shipped first (now in
 * that game's own repo): snapshot a model's own vertex buffer positions
 * once via t3d_vertbuffer_get_pos,
 * then write straight back into that SAME buffer in place, every frame,
 * with no segment redirection and no extra buffering. t3d_model_draw stays
 * completely unchanged at the call site. The only addition over the sea's
 * approach is scoping the walk to one named T3DObject's parts (via
 * t3d_model_get_object) rather than the whole model's shared vertex chunk,
 * so a crater on a "terrain" object never touches a sibling "gates" or
 * "tower" object sharing the same model.
 *
 * ── No separate heightmap data structure ─────────────────────────────────
 * The shipped mesh's own baked Y values ARE the base heightmap. A crater is
 * "subtract from a vertex's own already-baked Y, keyed by its own
 * already-baked (x,z), and let the subtraction decay to 0" — there is
 * nothing to re-derive from a terrain generator's own height function in C,
 * and so nothing that can drift from it. This is the n64-modeling skill's
 * "geometry measures itself" discipline, applied to a runtime effect
 * instead of a build-time one.
 *
 * ── Why touching every vertex per active crater is fine here ─────────────
 * Terrain meshes this module targets are low-poly by design (chunky-facet
 * console terrain, not a heightmap grid) — low thousands of triangles at
 * most. Walking every vertex of the target object within an active
 * crater's radius, every frame, with a zero-cost early-out when no crater
 * is active, is well inside budget. There is no dirty-rect/patch machinery
 * because the vertex count never gets large enough to need one.
 *
 * ── Collision is deliberately NOT here ────────────────────────────────────
 * kiln_crater_sample() is a pure query for a FUTURE kiln_room/kiln_clip
 * integration (a room's on_load could sample it while building brush
 * mins/maxs) — this module never calls into kiln_clip or kiln_room itself,
 * and no brush-mutation API is added to either. A terrain with no
 * collision today gets craters with no collision consequence; that is
 * correct, not a gap, until something actually walks on the terrain.
 */
#ifndef KILN_CRATER_H
#define KILN_CRATER_H

#include <stdint.h>

#include <t3d/t3dmodel.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KILN_CRATER_MAX_ACTIVE
#define KILN_CRATER_MAX_ACTIVE 8
#endif

/** One impact. `x`/`z` are world-space (the object's own local space, same
 *  convention as the model's baked vertex positions); the falloff and
 *  lifecycle are entirely in terms of these three: `radius` (world units,
 *  edge of effect), `depth_max` (world units, full-punch sink at the
 *  centre) and `age` (seconds since impact, drives the punch/heal curve
 *  below). `active` is 0 once fully healed. */
typedef struct {
    float x, z;
    float radius;
    float depth_max;
    float age;
    uint8_t active;
} KilnCraterSlot;

/** One vertex the field is allowed to mutate: a live pointer into the
 *  model's own resident buffer (via t3d_vertbuffer_get_pos), plus the rest
 *  pose read from it ONCE at init, before any mutation. Every frame's
 *  write is `rest.y0 - sink`, never an accumulated delta, so drift and
 *  rounding cannot build up across a long attract loop — the same reason
 *  the sea-swell pattern this module generalises writes absolute positions
 *  rather than deltas. */
typedef struct {
    int16_t *pos;          /* &T3DVertPacked.posA/posB[0..2], live         */
    int16_t x, y0, z;       /* rest pose, world units                       */
} KilnCraterVert;

typedef struct {
    const T3DModel *model;      /* borrowed */
    const T3DObject *object;    /* resolved once at init; borrowed         */
    KilnCraterVert *verts;       /* owned; vert_count entries                */
    int vert_count;

    KilnCraterSlot slots[KILN_CRATER_MAX_ACTIVE];

    int touched_last_update;    /* profiling: verts actually rewritten     */
} KilnCraterField;

/** Resolve `object_name` inside `model` (via t3d_model_get_object) and
 *  snapshot every vertex position across that object's parts. `model` must
 *  outlive the field. Returns 0 on success, -1 if the object was not found
 *  or has no vertices, -1 if the snapshot allocation failed (both leave the
 *  field zeroed / vert_count == 0, so kiln_crater_update becomes a no-op
 *  rather than a crash — a missing object is a boot-time fact worth a
 *  debugf, not a reason to take the whole scene down). */
int kiln_crater_init(KilnCraterField *cf, const T3DModel *model,
                    const char *object_name);

/** Frees the vertex snapshot. Does not touch `model` (caller-owned) and
 *  does not restore vertex positions — call this only when the object
 *  itself is going away (e.g. its model is being unloaded). */
void kiln_crater_destroy(KilnCraterField *cf);

/** Land an impact at local (x, z). If all KILN_CRATER_MAX_ACTIVE slots are
 *  already active, evicts the OLDEST (highest age) slot — a storm actively
 *  tearing up the ground reads better than silently dropping new strikes,
 *  and 8 concurrent craters is already generous for how fast they land
 *  (see the caller's own strike-gap timing). A no-op if vert_count == 0. */
void kiln_crater_impact(KilnCraterField *cf, float x, float z,
                       float radius, float depth_max);

/** Recompute every active crater's current depth and rewrite the affected
 *  vertices' Y in place (see KilnCraterVert's comment on why this writes
 *  absolute positions, not deltas). Zero cost when no crater is active —
 *  a single flag check, no vertex walk. Overlapping craters at one vertex
 *  combine by MAX, not sum, so two nearby strikes don't dig an unrealistic
 *  pit. Call once per frame, before the model's own draw call. */
void kiln_crater_update(KilnCraterField *cf, float dt);

/** Pure query: current total sink (world units, >= 0) at local (x, z), by
 *  the same falloff kiln_crater_update uses — recomputed from the slot
 *  table, not read back from a vertex (so it works for any (x,z), not just
 *  ones that happen to sit on a vertex). 0 if no crater reaches this point.
 *  Not called anywhere in this module's own callers yet; exists for a
 *  future kiln_room/kiln_clip integration. */
float kiln_crater_sample(const KilnCraterField *cf, float x, float z);

#ifdef __cplusplus
}
#endif

#endif /* KILN_CRATER_H */
