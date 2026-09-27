/* SPDX-License-Identifier: MIT
 *
 * kiln_sierp.h — Sierpinski tetrahedron (tetrix) as four-corner leaves.
 *
 * Subdivision is iterative, in place, from the back of the buffer: each
 * parent becomes four corner children, no recursion, no scratch buffer.
 * Depth d needs 4^d slots. Morph is a per-vertex lerp so two orientations
 * of the same tree can turn into each other without rebuilding topology.
 */
#ifndef FIG_SIERP_H
#define FIG_SIERP_H

#include <t3d/t3dmath.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIG_SIERP_MAX_DEPTH  4
#define FIG_SIERP_MAX_LEAVES 256 /* 4^4 */

typedef struct FigTet {
    fm_vec3_t v[4];
} FigTet;

/** Regular tet centred on the origin, circumradius `radius`. */
void fig_sierp_regular(FigTet *out, float radius);

/** Rotate about Y by a given cos/sin (caller computes once per pose). */
void fig_sierp_rotate_y(FigTet *out, const FigTet *in, float c, float s);

/** Pointwise negate — the dual orientation through the origin. */
void fig_sierp_negate(FigTet *out, const FigTet *in);

/** Fill `out` with the 4^depth corner tets of `root`. Returns the count,
 *  or 0 if `cap` cannot hold depth 0. Stops at the last depth that fits. */
int fig_sierp_leaves(FigTet *out, int cap, const FigTet *root, int depth);

/** dst[i] = lerp(a[i], b[i], t). t is clamped to [0,1]. */
void fig_sierp_morph(FigTet *dst, const FigTet *a, const FigTet *b,
                      int n, float t);

/** Cubic smoothstep, clamped. */
float fig_sierp_smooth(float t);

#ifdef __cplusplus
}
#endif

#endif /* FIG_SIERP_H */
