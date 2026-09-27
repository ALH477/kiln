/* SPDX-License-Identifier: MIT
 *
 * kiln_pose.h — the arithmetic fig_skel does to bones, as pure functions.
 *
 * fig_skel's overlay slot blends a one-shot clip (an attack, a wave) over
 * the locomotion pose for only SOME bones — an upper body swinging a sword
 * while the legs keep running. Tiny3D's t3d_skeleton_blend blends every bone,
 * so that masked blend is this engine's own code, and code of exactly the
 * kind that renders plausibly when wrong: a mask off by one bone is an arm
 * that half-follows the attack, and a quaternion blend that takes the long
 * way round is a wrist that spins a full turn mid-swing. Neither is
 * distinguishable from an animation authoring mistake in a capture.
 *
 * So it lives here, over plain T3DBone arrays and nothing else — no model,
 * no skeleton, no RSP — which is what lets nix/checks/kiln-pose.nix compile
 * it natively and assert on it. The host cannot run a skeleton at all
 * (plat/host aborts on every t3d_skeleton_* call); it can run this.
 *
 * ── Conventions ──────────────────────────────────────────────────────────
 * Quaternions are Tiny3D's: v[0..2] = x, y, z and v[3] = w, so identity is
 * {0, 0, 0, 1}. Bones are stored DEPTH-FIRST — Tiny3D's own
 * t3d_skeleton_update relies on it (a changed bone re-evaluates every
 * following bone deeper than it), and the model writer emits them that way —
 * so a bone's subtree is the bone plus the run of following bones deeper
 * than it, and a subtree mask needs nothing but the depth column.
 */
#ifndef FIG_POSE_H
#define FIG_POSE_H


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
#include <t3d/t3dmath.h>
#include <t3d/t3dskeleton.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A mask bit per bone for the first 32 bones. fig_skel asserts a rig has
 *  no more than this; the pure functions below treat a bone past the 32nd as
 *  masked in only when the mask is FIG_POSE_MASK_ALL. */
#define FIG_POSE_MAX_BONES 32
#define FIG_POSE_MASK_ALL  0xFFFFFFFFu

/** Normalised lerp, taking the short way round (negates `b`'s contribution
 *  when the two are more than a half-turn apart). The same arithmetic as
 *  Tiny3D's t3d_quat_nlerp, which is what t3d_skeleton_blend and
 *  t3d_anim_update use — so a masked blend and an unmasked one agree about
 *  what "halfway" means. `out` may alias either input. */
void fig_quat_nlerp(T3DQuat *out, const T3DQuat *a, const T3DQuat *b, float t);

/** Hamilton product a * b (apply b, then a). `out` may alias either input. */
void fig_quat_mul(T3DQuat *out, const T3DQuat *a, const T3DQuat *b);

/** Rotation of `angle` radians about the axis (ax, ay, az), which must be
 *  unit length. */
void fig_quat_axis_angle(T3DQuat *out, float ax, float ay, float az, float angle);

/** Blend `over` into `base` by weight `w`, for masked bones only.
 *
 *  A bone whose mask bit is set gets rotation nlerp'd, position and scale
 *  lerp'd, and `hasChanged` raised so t3d_skeleton_update rebuilds its
 *  matrix (and every child's). A bone whose bit is clear — or every bone,
 *  when w <= 0 — is left exactly as `base` has it, `hasChanged` included.
 *  `out` may alias `base`; `matrix` is never touched. */
void fig_pose_blend_masked(T3DBone *out, const T3DBone *base, const T3DBone *over,
                            int n, uint32_t mask, float w);

/** The mask of `root` and its whole subtree, from a depth-first array of bone
 *  depths. 0 for an out-of-range root. */
uint32_t fig_pose_subtree_mask(const uint16_t *depth, int n, int root);

#ifdef __cplusplus
}
#endif

#endif /* FIG_POSE_H */
