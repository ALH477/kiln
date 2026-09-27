/* SPDX-License-Identifier: MIT
 *
 * plat/host/include/t3d/t3dmath.h — the host's <t3d/t3dmath.h>.
 *
 * Tiny3D's real t3dmath.h is 603 lines and almost all of it is portable, but
 * its first line is `#include <libdragon.h>`, so it cannot be used until the
 * host libdragon surface exists. Until then this stands in for it.
 *
 * ── What makes this honest ─────────────────────────────────────────────
 * It defines NOTHING. Tiny3D's own line is
 *
 *     typedef fm_vec3_t T3DVec3;
 *
 * — the coupling CLAUDE.md records as the reason libdragon is pinned to
 * `preview` — and `fm_vec3_t` now comes from libdragon's actual fgeom.h,
 * compiled natively by nix/host-math.nix. So the type the host sees is not a
 * reproduction of the console's, it IS the console's, down to the last bit of
 * the polynomial approximations behind fm_sinf.
 *
 * That is the whole difference from what this replaced. The old
 * nix/checks/stub/t3d/t3dmath.h carried a hand-copy of the union and of
 * fm_vec3_sub, with a header explaining that a copy was the only honest way to
 * fake it and that anything approximated "will make a host check disagree with
 * the console for reasons that have nothing to do with the code being tested."
 * Nothing is copied now, so nothing can drift.
 *
 * Both spellings of the union — `.x/.y/.z` and `.v[i]` — are load-bearing:
 * kiln_clip.c indexes `.v[i]` in its slab loops and callers across the engine
 * write `{{ x, y, z }}` initialisers. They come from one definition, so they
 * cannot disagree.
 */
#ifndef FIG_HOST_T3DMATH_H
#define FIG_HOST_T3DMATH_H

#include <fgeom.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* Verbatim from Tiny3D's t3dmath.h. */
typedef fm_vec3_t T3DVec3;
typedef fm_vec4_t T3DVec4;
typedef fm_quat_t T3DQuat;
typedef fm_mat4_t T3DMat4;

/* Verbatim INCLUDING the missing parentheses around `deg`. Tiny3D's macro is
 * unparenthesised, so T3D_DEG_TO_RAD(a + b) means a + (b * k) upstream. Both
 * call sites in this tree pass a plain lvalue (kiln_engine.c:106,227) so it is
 * latent, but reproducing it is the point: a host header that quietly fixed
 * an upstream bug would stop predicting what the console does, which is the
 * same reason the host text layer keeps libdragon's missing '@' glyph
 * missing. If this ever wants fixing, fix it in Tiny3D. */
#define T3D_DEG_TO_RAD(deg) (deg * 0.01745329252f)

/* ── The view frustum ─────────────────────────────────────────────────────
 * Verbatim from Tiny3D: six planes, each (nx, ny, nz, d), normalised. The
 * shape matters as much as the maths — fig_cull hands a world-space sphere
 * straight to t3d_frustum_vs_sphere, and a host that stored planes any other
 * way would have to convert on every test.
 */
typedef struct { T3DVec4 planes[6]; } T3DFrustum;

/** Extract the six planes from a combined view-projection matrix.
 *  Copied from Tiny3D's t3dmath.c, including the normalisation pass — a host
 *  that skipped it would make `dist` a scaled quantity and the radius
 *  comparison in vs_sphere would be wrong by that scale. */
void t3d_mat4_to_frustum(T3DFrustum *frustum, const T3DMat4 *mat);

/** True when any part of the sphere is inside. Conservative: a sphere the
 *  planes all pass is kept even if it is outside a corner. */
bool t3d_frustum_vs_sphere(const T3DFrustum *frustum, const T3DVec3 *center,
                           float radius);

/* ── The bone maths ───────────────────────────────────────────────────────
 * Seven helpers, copied from Tiny3D's t3dmath.{h,c} rather than rederived,
 * because t3d_skeleton_update is now implemented here and the host's bind
 * pose has to be the console's bind pose to the bit. kiln_pose.c already made
 * this argument for one of them — its comment reads "Tiny3D's t3d_quat_nlerp
 * (src/t3d/t3dmath.c), line for line" — and it had to reimplement it because
 * these were not declared. They are now, so the next module does not.
 *
 * The inline/out-of-line split is upstream's, kept so a caller that takes an
 * address of one behaves the same on both targets: t3d_quat_nlerp and
 * t3d_mat4_from_srt live in t3dmath.c there and in host_t3d.c here; the rest
 * are `inline static` in the header there and here.
 */
inline static void t3d_vec3_lerp(T3DVec3 *res, const T3DVec3 *a, const T3DVec3 *b, float t)
{
  res->v[0] = a->v[0] + (b->v[0] - a->v[0]) * t;
  res->v[1] = a->v[1] + (b->v[1] - a->v[1]) * t;
  res->v[2] = a->v[2] + (b->v[2] - a->v[2]) * t;
}

inline static float t3d_quat_dot(const T3DQuat *a, const T3DQuat *b)
{
  return a->v[0] * b->v[0] + a->v[1] * b->v[1] + a->v[2] * b->v[2] + a->v[3] * b->v[3];
}

inline static void t3d_quat_normalize(T3DQuat *quat)
{
  float scale = 1.0f / sqrtf(quat->v[0]*quat->v[0] + quat->v[1]*quat->v[1] + quat->v[2]*quat->v[2] + quat->v[3]*quat->v[3]);
  quat->v[0] *= scale;
  quat->v[1] *= scale;
  quat->v[2] *= scale;
  quat->v[3] *= scale;
}

inline static void t3d_mat4_scale(T3DMat4 *mat, float scaleX, float scaleY, float scaleZ)
{
  mat->m[0][0] *= scaleX;
  mat->m[0][1] *= scaleX;
  mat->m[0][2] *= scaleX;
  mat->m[0][3] *= scaleX;

  mat->m[1][0] *= scaleY;
  mat->m[1][1] *= scaleY;
  mat->m[1][2] *= scaleY;
  mat->m[1][3] *= scaleY;

  mat->m[2][0] *= scaleZ;
  mat->m[2][1] *= scaleZ;
  mat->m[2][2] *= scaleZ;
  mat->m[2][3] *= scaleZ;
}

inline static void t3d_mat4_mul(T3DMat4 *matRes, const T3DMat4 *matA, const T3DMat4 *matB)
{
  for(uint32_t i=0; i<4; i++) {
    for(uint32_t  j=0; j<4; j++) {
      matRes->m[j][i] = matA->m[0][i] * matB->m[j][0] +
                        matA->m[1][i] * matB->m[j][1] +
                        matA->m[2][i] * matB->m[j][2] +
                        matA->m[3][i] * matB->m[j][3];
    }
  }
}

/** Normalised lerp, not slerp. t3d_skeleton_blend and t3d_anim_update both
 *  use this one, so a host that reached for slerp would disagree with the
 *  console on every blended frame by a little. */
void t3d_quat_nlerp(T3DQuat *res, const T3DQuat *a, const T3DQuat *b, float t);

/** Scale-rotate-translate, column-major, translation in row 3. */
void t3d_mat4_from_srt(T3DMat4 *mat, const float scale[3], const float quat[4],
                       const float translate[3]);

#endif /* FIG_HOST_T3DMATH_H */
