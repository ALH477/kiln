/* SPDX-License-Identifier: MIT
 *
 * kiln_sierp.h — Sierpinski tetrahedron (tetrix) as four-corner leaves.
 *
 * Subdivision is iterative, in place, from the back of the buffer: each
 * parent becomes four corner children, no recursion, no scratch buffer.
 * Depth d needs 4^d slots. Morph is a per-vertex lerp so two orientations
 * of the same tree can turn into each other without rebuilding topology.
 */
#ifndef KILN_SIERP_H
#define KILN_SIERP_H

#include <t3d/t3dmath.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KILN_SIERP_MAX_DEPTH  4
#define KILN_SIERP_MAX_LEAVES 256 /* 4^4 */

typedef struct KilnTet {
    fm_vec3_t v[4];
} KilnTet;

/** Regular tet centred on the origin, circumradius `radius`. */
void kiln_sierp_regular(KilnTet *out, float radius);

/** Rotate about Y by a given cos/sin (caller computes once per pose). */
void kiln_sierp_rotate_y(KilnTet *out, const KilnTet *in, float c, float s);

/** Pointwise negate — the dual orientation through the origin. */
void kiln_sierp_negate(KilnTet *out, const KilnTet *in);

/** Fill `out` with the 4^depth corner tets of `root`. Returns the count,
 *  or 0 if `cap` cannot hold depth 0. Stops at the last depth that fits. */
int kiln_sierp_leaves(KilnTet *out, int cap, const KilnTet *root, int depth);

/** dst[i] = lerp(a[i], b[i], t). t is clamped to [0,1]. */
void kiln_sierp_morph(KilnTet *dst, const KilnTet *a, const KilnTet *b,
                      int n, float t);

/** Cubic smoothstep, clamped. */
float kiln_sierp_smooth(float t);

#ifdef __cplusplus
}
#endif

#endif /* KILN_SIERP_H */
