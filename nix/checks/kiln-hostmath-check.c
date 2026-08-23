/* SPDX-License-Identifier: MIT
 *
 * Does the host build of libdragon's fast math still mean what the console's
 * does? nix/host-math.nix substitutes four MIPS instructions for the libm
 * calls they are documented as optimising. This asserts that claim instead of
 * trusting the documentation, because everything downstream — every screen
 * position, every collision epsilon — is computed through these.
 */
#include <fgeom.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { \
        printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    /* ── the four patched functions, against the libm they replaced ──
     * Swept rather than spot-checked: the interesting inputs are the exact
     * integers and the halfway points, where a tie-breaking rule shows up. */
    for (int i = -2000; i <= 2000; i++) {
        float x = (float)i * 0.25f;          /* hits .0 .25 .5 .75 exactly */
        CHECK(fm_floorf(x) == floorf(x), "fm_floorf(%.2f) = %.2f, floorf = %.2f",
              x, fm_floorf(x), floorf(x));
        CHECK(fm_ceilf(x)  == ceilf(x),  "fm_ceilf(%.2f) = %.2f, ceilf = %.2f",
              x, fm_ceilf(x), ceilf(x));
        CHECK(fm_truncf(x) == truncf(x), "fm_truncf(%.2f) = %.2f, truncf = %.2f",
              x, fm_truncf(x), truncf(x));
        /* MIPS round.w.s breaks ties to EVEN, so nearbyintf is the match and
         * roundf (ties away from zero) is NOT. Assert both halves of that, or
         * the substitution could be wrong in the one place it matters. */
        CHECK(fm_roundf(x) == nearbyintf(x),
              "fm_roundf(%.2f) = %.2f, nearbyintf = %.2f",
              x, fm_roundf(x), nearbyintf(x));
    }
    CHECK(fm_roundf(0.5f) == 0.0f, "ties must go to even: fm_roundf(0.5) = %.1f",
          fm_roundf(0.5f));
    CHECK(fm_roundf(1.5f) == 2.0f, "ties must go to even: fm_roundf(1.5) = %.1f",
          fm_roundf(1.5f));
    CHECK(fm_roundf(2.5f) == 2.0f, "ties must go to even: fm_roundf(2.5) = %.1f",
          fm_roundf(2.5f));

    /* ── fm_vec3_t's layout, which 392 sites depend on ──
     * `.x/.y/.z` and `.v[i]` must name the same three floats: kiln_clip.c
     * indexes .v[i] in its slab loops while callers write {{x,y,z}}. A type
     * carrying only one spelling would compile the engine and change what it
     * means. */
    fm_vec3_t a = {{ 1.0f, 2.0f, 3.0f }};
    CHECK(a.v[0] == a.x && a.v[1] == a.y && a.v[2] == a.z,
          "fm_vec3_t union spellings disagree");
    CHECK(sizeof(fm_vec3_t) == 3 * sizeof(float),
          "fm_vec3_t is %zu bytes, expected %zu", sizeof(fm_vec3_t),
          3 * sizeof(float));

    /* ── the real implementations are present and sane ──
     * These are libdragon's polynomial approximations, not libm, so they are
     * checked against a tolerance. The point is that they are LINKED at all:
     * fm_sinf/cosf/atan2f are out-of-line in src/math/fmath.c, which is what
     * the host derivation has to compile for any of this to work. */
    for (int d = -360; d <= 360; d += 3) {
        float r = (float)d * 0.01745329252f;
        CHECK(fabsf(fm_sinf(r) - sinf(r)) < 1e-3f,
              "fm_sinf(%d deg) = %.5f, sinf = %.5f", d, fm_sinf(r), sinf(r));
        CHECK(fabsf(fm_cosf(r) - cosf(r)) < 1e-3f,
              "fm_cosf(%d deg) = %.5f, cosf = %.5f", d, fm_cosf(r), cosf(r));
    }
    CHECK(fabsf(fm_atan2f(1.0f, 2.0f) - atan2f(1.0f, 2.0f)) < 1e-3f,
          "fm_atan2f(1,2) = %.5f, atan2f = %.5f",
          fm_atan2f(1.0f, 2.0f), atan2f(1.0f, 2.0f));

    /* Normalisation, the one the camera and every direction vector use. */
    fm_vec3_t n; fm_vec3_norm(&n, &a);
    CHECK(fabsf(fm_vec3_len(&n) - 1.0f) < 1e-4f,
          "normalised length is %.6f", fm_vec3_len(&n));

    if (fails) { printf("\nFAILED (%d)\n", fails); return 1; }
    printf("host fast-math matches the console's: floor/ceil/trunc/round exact, "
           "sin/cos/atan2 linked, fm_vec3_t layout intact\n");
    return 0;
}
