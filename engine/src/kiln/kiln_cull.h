/* SPDX-License-Identifier: MIT
 *
 * kiln_cull.h — the arithmetic of not drawing something.
 *
 * Pure: no Tiny3D types, no RDP, no RSP. This is the half of per-object
 * culling that can be asserted on the host, which is why it is a separate
 * unit from kiln_detail.c — the same split fig_voxel/fig_voxmesh already
 * uses, and for the same reason (nix/checks/kiln-logic.nix's header).
 *
 * ── What this is for ───────────────────────────────────────────────────
 * A T3DObject carries its own bounding box, as six int16s in MODEL space
 * (t3dmodel.h: aabbMin/aabbMax), and Tiny3D ships a frustum test that wants a
 * sphere in WORLD space (t3d_frustum_vs_sphere, t3dmath.c:140). The gap
 * between those two is this file.
 *
 * ── Why a sphere, and not the box ──────────────────────────────────────
 * Tiny3D also has t3d_frustum_vs_aabb_s16, which is tighter, and its own
 * culling example uses it — but only by transforming the FRUSTUM into model
 * space, and its comment is careful to say why that was affordable there:
 *
 *     "since we want to avoid transforming individual AABBs, we transform the
 *      frustum to match our map instead (model space). In this case we only
 *      have to scale it"
 *
 * That works when a model is drawn at the origin with a uniform scale, which
 * is the case in that example and is not the case in a game: objects are drawn
 * at a position, with a yaw, several instances of one model per frame. There is
 * no t3d_frustum_translate or t3d_frustum_rotate to answer that with, and
 * inverse-transforming six planes per instance is more work than the test
 * saves.
 *
 * A sphere has the property that makes this cheap instead: it is invariant
 * under rotation. So the world-space sphere of a placed object needs the
 * position, the scale and — only for the centre, not the radius — the yaw.
 * Six dot products later, Tiny3D's own plane test gives the answer.
 *
 * ── Conservative, deliberately ─────────────────────────────────────────
 * A bounding sphere of a box is larger than the box, so this rejects strictly
 * less than a perfect test would. That is the right direction to be wrong in:
 * a too-generous test costs a few triangles that were going to be drawn
 * anyway, and a too-eager one deletes something the player was looking at.
 */
#ifndef FIG_CULL_H
#define FIG_CULL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Detail tier encoded in an object's name: `torso.lod2` -> 2.
 *
 *  Anything that is not exactly `.lod` followed by digits is tier 0 — in
 *  particular `torso.001`, which is Blender's own suffix for a name collision
 *  and emphatically not a tier. Reading that as one would drop real geometry
 *  out of the frame the moment two objects were given the same name, and the
 *  symptom would be a hole in the model rather than an error.
 *
 *  Returns 0 for NULL.
 */
uint8_t fig_cull_tier_of(const char *name);

/** The world-space bounding sphere of a model-space AABB, placed.
 *
 *  `aabb_min`/`aabb_max` are a T3DObject's own int16 bounds. `scale` is
 *  uniform — for a non-uniform scale, pass the largest axis, which keeps the
 *  sphere conservative. `yaw` is radians about +Y, in the engine's convention:
 *  yaw 0 faces +Z and forward is (sin yaw, 0, cos yaw), the same one
 *  fig_fpscam documents and pm_demo.c places its props with.
 *
 *  `out_centre` may not alias `pos`.
 */
void fig_cull_sphere_of(const int16_t aabb_min[3], const int16_t aabb_max[3],
                         const float pos[3], float scale, float yaw,
                         float out_centre[3], float *out_radius);

/** Squared distance from `eye` to the SURFACE of a placed object's bounding
 *  sphere — zero when the eye is inside it.
 *
 *  This is the distance a detail tier should be chosen on, and the difference
 *  from "distance to its origin" is not a refinement: it is the difference
 *  between a correct answer and a nonsensical one for anything large. The
 *  island in PetaByte Madness has a radius of about 13,800 units, so a camera
 *  3,400 units from its centre is INSIDE it — while a naive centre-distance
 *  would read 3,400 and pick a low tier for the object filling the screen.
 *
 *  Squared, so the caller can feed it straight to fig_lod_select without a
 *  square root in the per-instance path.
 */
float fig_cull_surface_dist_sq(const int16_t aabb_min[3],
                                const int16_t aabb_max[3],
                                const float pos[3], float scale, float yaw,
                                const float eye[3]);

#ifdef __cplusplus
}
#endif

#endif /* FIG_CULL_H */
