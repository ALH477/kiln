// SPDX-License-Identifier: MIT
//
// kiln_camkey.h — one camera keyframe, and the curve through a table of them.
//
// Originated in a downstream game (now in that game's own repo) and moved
// into the engine when a second consumer appeared: Forge's CAM mode authors
// these tables, and an editor that previews a curve merely resembling the
// one a game flies is the same failure as a validator measuring one — worse,
// because the author then tunes against the wrong shape. That game keeps a
// shim over its old header path so KilnCamKey/kiln_camkey_sample keep
// working unchanged for it.
//
// Split out of the runtime's own camera-table header so consumers can share
// one implementation of the interpolation instead of several that agree by
// inspection — a game's runtime (the camera the player sees), a game's own
// spatial-overlay debugger (which draws the flown curve), the static
// validator (`kiln_camlint`, which measures it), and Forge (the editor,
// which authors the table in the first place).
//
// The validator is why this header exists at all: it is deliberately free
// of libdragon and Tiny3D so it compiles natively in `nix flake check` (the
// pattern nix/checks/kiln-logic.nix proves), while a runtime's own camera
// code is not — it pulls in kiln_camera.h and kiln_engine.h. Rather than let
// the validator carry its own copy of the spline, the spline moved here,
// where it needs nothing but `fm_vec3_t`.
//
// That matters more than it sounds. A validator measuring overshoot on a curve
// that is merely SIMILAR to the one the camera flies is worse than no
// validator: it reports numbers that look authoritative about a curve nobody
// renders. Sharing the function makes the two identical by construction.
//
// ── Why static inline rather than a .c ─────────────────────────────────
// It is one small function called once a frame by the runtime and a few hundred
// times by the validator, and inlining it removes every question about which
// object file each consumer links. The alternative — a kiln_camkey.c — would
// have to be added to Forge/Makefile's OBJS and the native check compile
// lines in every project that uses it, i.e. more places to forget with each
// new consumer.
//
// That is also why engine/Makefile grew a HEADER_ONLY list rather than putting
// this in MODULES: MODULES drives $(OBJS), and a name in there with no .c fails
// the archive outright.

#ifndef KILN_CAMKEY_H
#define KILN_CAMKEY_H

#include <t3d/t3dmath.h>

/** One camera keyframe. `t` is seconds from the start of the shot. */
typedef struct {
    float     t;
    fm_vec3_t eye;
    fm_vec3_t look;
} KilnCamKey;

/** Catmull-Rom through four control values, evaluated at `f` in [0,1] between
 *  p1 and p2.
 *
 *  Passes exactly through every key and, unlike a per-segment ease, has a
 *  CONTINUOUS velocity across them — which is the whole point here. */
static inline float kiln_camkey_spline1(float p0, float p1, float p2, float p3,
                                      float f)
{
    const float f2 = f * f;
    const float f3 = f2 * f;
    return 0.5f * ((2.0f * p1)
                 + (-p0 + p2) * f
                 + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * f2
                 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * f3);
}

/** Eye and look at shot time `t`. `loop` selects the wrap-around neighbour
 *  policy (see below); either output pointer may be NULL.
 *
 *  ── Catmull-Rom, not a per-segment ease ─────────────────────────────────
 *  This used to smoothstep `f` and lerp between the two bracketing keys.
 *  Smoothstep starts and ends at zero velocity, so the camera decelerated to a
 *  near-stop at EVERY key and accelerated away from it again — on the flyover's
 *  fourteen keys over thirty-six seconds that is a hitch every two and a half
 *  seconds, which reads as a clunky move rather than a shot.
 *
 *  Catmull-Rom needs the keys either side of the segment as well, and gives a
 *  curve that passes through every key with a continuous velocity through it.
 *  The interpolant stays LINEAR in f: easing it would put the per-key
 *  deceleration straight back.
 *
 *  A looping shot wraps for its neighbours, so the seam is as smooth as
 *  anywhere else — a shot that ends on a copy of its first key should skip
 *  that duplicate in the wrap. A one-shot clamps at the ends instead,
 *  which makes the tangent zero there: a natural ease in and out at the START
 *  and END of the shot only, which is what a cut wants.
 *
 *  The price, and the reason the validator measures it: the tangent at a key
 *  comes from that key's TWO NEIGHBOURS, so a large gap next to a small one
 *  drags the curve past the small one. A real shot has needed three
 *  hand-inserted midpoint keys whose only job was suppressing exactly that. */
static inline void kiln_camkey_sample(const KilnCamKey *keys, int n, int loop,
                                    float t, fm_vec3_t *eye, fm_vec3_t *look)
{
    if (!keys || n <= 0) return;

    /* Bracketing pair. Same linear scan examples/cinematic-demo uses: a
     * handful of keys per shot, walked once a frame. */
    int i = 0;
    while (i < n - 2 && t >= keys[i + 1].t) i++;

    const int i1 = i;
    const int i2 = (i + 1 < n) ? i + 1 : i;
    const float span = keys[i2].t - keys[i1].t;
    float f = span > 0.0f ? (t - keys[i1].t) / span : 0.0f;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;

    int i0, i3;
    if (loop && n >= 3) {
        i0 = (i1 > 0) ? i1 - 1 : n - 2;          /* n-1 duplicates key 0 */
        i3 = (i2 + 1 < n) ? i2 + 1 : 1;
    } else {
        i0 = (i1 > 0) ? i1 - 1 : i1;
        i3 = (i2 + 1 < n) ? i2 + 1 : i2;
    }

    for (int k = 0; k < 3; k++) {
        if (eye)
            eye->v[k] = kiln_camkey_spline1(keys[i0].eye.v[k], keys[i1].eye.v[k],
                                          keys[i2].eye.v[k], keys[i3].eye.v[k],
                                          f);
        if (look)
            look->v[k] = kiln_camkey_spline1(keys[i0].look.v[k],
                                           keys[i1].look.v[k],
                                           keys[i2].look.v[k],
                                           keys[i3].look.v[k], f);
    }
}

#endif // KILN_CAMKEY_H
