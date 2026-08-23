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
#ifndef KILN_HOST_T3DMATH_H
#define KILN_HOST_T3DMATH_H

#include <fgeom.h>

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

#endif /* KILN_HOST_T3DMATH_H */
