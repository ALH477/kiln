/* SPDX-License-Identifier: MIT
 *
 * kiln-pose-check.c — host assertions over kiln_pose, the arithmetic behind
 * kiln_skel's masked overlay blend and bone overrides.
 *
 * Driven by nix/checks/kiln-pose.nix. Every property here is one whose
 * failure renders as a plausible animation on console: a mask one bone wide
 * of the torso is an arm that half-follows an attack, and a quaternion blend
 * that takes the long way round is a wrist that spins a turn mid-swing. The
 * host cannot run a skeleton, so this is the only place either can fail.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kiln_pose.h"

static int g_fail;

static void ok(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs(cond ? "  ok   " : "  FAIL ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    if (!cond) g_fail++;
}

#define NEAR(a, b) (fabsf((a) - (b)) <= 1e-4f)

static T3DQuat Q(float x, float y, float z, float w) { return (T3DQuat){{ x, y, z, w }}; }

static float quat_angle(const T3DQuat *q)
{
    /* Rotation angle, sign-insensitive: q and -q are the same rotation. */
    const float w = fabsf(q->v[3]);
    return 2.0f * acosf(w > 1.0f ? 1.0f : w);
}

static void test_quat(void)
{
    puts("quaternions");
    const float half = (float)M_SQRT1_2;
    const T3DQuat id = Q(0, 0, 0, 1);
    const T3DQuat y90 = Q(0, half, 0, half);

    T3DQuat r;
    kiln_quat_nlerp(&r, &id, &y90, 0.5f);
    ok(NEAR(quat_angle(&r), (float)M_PI / 4.0f), "identity -> 90 deg at 0.5 is 45 deg (got %.3f rad)",
       quat_angle(&r));
    ok(r.v[1] > 0.0f, "... about +Y");

    /* -y90 is the same rotation as y90. A naive lerp from identity towards it
     * passes through a 180-degree detour; the short path must match y90's. */
    const T3DQuat y90n = Q(0, -half, 0, -half);
    T3DQuat r2;
    kiln_quat_nlerp(&r2, &id, &y90n, 0.5f);
    ok(NEAR(quat_angle(&r2), (float)M_PI / 4.0f), "antipodal target takes the short path (got %.3f rad)",
       quat_angle(&r2));

    kiln_quat_nlerp(&r, &id, &y90, 0.0f);
    ok(NEAR(r.v[3], 1.0f), "t = 0 returns a");
    kiln_quat_nlerp(&r, &id, &y90, 1.0f);
    ok(NEAR(fabsf(r.v[1]), half) && NEAR(fabsf(r.v[3]), half), "t = 1 returns b");

    T3DQuat a;
    kiln_quat_axis_angle(&a, 0, 1, 0, (float)M_PI / 2.0f);
    ok(NEAR(a.v[1], half) && NEAR(a.v[3], half), "axis_angle(+Y, 90) == y90");

    T3DQuat m;
    kiln_quat_mul(&m, &y90, &y90);
    ok(NEAR(fabsf(m.v[1]), 1.0f) && NEAR(m.v[3], 0.0f), "y90 * y90 == y180");
    kiln_quat_mul(&m, &id, &y90);
    ok(NEAR(m.v[1], half) && NEAR(m.v[3], half), "identity * q == q");

    /* Non-commutative: rotating about X then Y is not Y then X. A mul that
     * swapped its operands would pass every single-axis case above. */
    T3DQuat x90, xy, yx;
    kiln_quat_axis_angle(&x90, 1, 0, 0, (float)M_PI / 2.0f);
    kiln_quat_mul(&xy, &x90, &y90);
    kiln_quat_mul(&yx, &y90, &x90);
    ok(!NEAR(xy.v[2], yx.v[2]), "mul is ordered (x*y z=%.3f, y*x z=%.3f)", xy.v[2], yx.v[2]);
    /* Hamilton: (i + 1)(j + 1)/2 has z = +1/2. */
    ok(NEAR(xy.v[2], 0.5f), "x90 * y90 has z = +0.5 (Hamilton order)");

    kiln_quat_mul(&m, &y90, &y90);
    kiln_quat_mul(&m, &m, &y90);   /* aliased output */
    ok(NEAR(quat_angle(&m), (float)M_PI / 2.0f), "aliased mul: y90^3 is a 90-degree rotation");
}

/* goblin.py's rig, in export order: depth-first. */
enum { ROOT, TORSO, NECK, HEAD, ARM_L, HAND_L, ARM_R, HAND_R, THIGH_L, SHIN_L, THIGH_R, SHIN_R, NB };
static const uint16_t DEPTH[NB] = { 0, 1, 2, 3, 2, 3, 2, 3, 1, 2, 1, 2 };

static void bone(T3DBone *b, float rot_y, float px)
{
    memset(b, 0, sizeof(*b));
    kiln_quat_axis_angle(&b->rotation, 0, 1, 0, rot_y);
    b->position = (T3DVec3){{ px, 0, 0 }};
    b->scale = (T3DVec3){{ 1, 1, 1 }};
    b->matrix.m[3][3] = 7.0f;   /* a marker the blend must never touch */
}

static void test_mask(void)
{
    puts("subtree masks");
    const uint32_t torso = kiln_pose_subtree_mask(DEPTH, NB, TORSO);
    const uint32_t want = (1u << TORSO) | (1u << NECK) | (1u << HEAD) | (1u << ARM_L) | (1u << HAND_L) |
                          (1u << ARM_R) | (1u << HAND_R);
    ok(torso == want, "torso subtree is torso, neck, head, both arms and hands (0x%03x, want 0x%03x)",
       torso, want);
    ok(!(torso & (1u << ROOT)) && !(torso & (1u << THIGH_L)), "... and not the root or the legs");
    ok(kiln_pose_subtree_mask(DEPTH, NB, ARM_R) == ((1u << ARM_R) | (1u << HAND_R)),
       "arm_r subtree stops at its sibling (thigh_l)");
    ok(kiln_pose_subtree_mask(DEPTH, NB, ROOT) == (1u << NB) - 1, "root subtree is every bone");
    ok(kiln_pose_subtree_mask(DEPTH, NB, SHIN_R) == (1u << SHIN_R), "the last bone is its own subtree");
    ok(kiln_pose_subtree_mask(DEPTH, NB, -1) == 0 && kiln_pose_subtree_mask(DEPTH, NB, NB) == 0,
       "an unknown bone masks nothing");
}

static void test_blend(void)
{
    puts("masked blend");
    T3DBone base[NB], over[NB], out[NB];
    for (int i = 0; i < NB; i++) {
        bone(&base[i], 0.0f, 1.0f);
        bone(&over[i], (float)M_PI / 2.0f, 3.0f);
        base[i].hasChanged = 0;
    }
    const uint32_t mask = kiln_pose_subtree_mask(DEPTH, NB, TORSO);

    kiln_pose_blend_masked(out, base, over, NB, mask, 1.0f);
    int masked_ok = 1, unmasked_ok = 1;
    for (int i = 0; i < NB; i++) {
        if (mask & (1u << i)) {
            masked_ok &= NEAR(quat_angle(&out[i].rotation), (float)M_PI / 2.0f) &&
                         NEAR(out[i].position.v[0], 3.0f) && out[i].hasChanged == 1;
        } else {
            unmasked_ok &= memcmp(&out[i], &base[i], sizeof(T3DBone)) == 0;
        }
    }
    ok(masked_ok, "w = 1: masked bones take the overlay and are flagged changed");
    ok(unmasked_ok, "w = 1: unmasked bones are bit-identical to the base");

    kiln_pose_blend_masked(out, base, over, NB, mask, 0.5f);
    ok(NEAR(quat_angle(&out[HEAD].rotation), (float)M_PI / 4.0f) && NEAR(out[HEAD].position.v[0], 2.0f),
       "w = 0.5: halfway in rotation (45 deg) and position (2.0)");
    ok(out[HEAD].matrix.m[3][3] == 7.0f, "the bone matrix is left for t3d_skeleton_update");

    kiln_pose_blend_masked(out, base, over, NB, mask, 0.0f);
    ok(memcmp(out, base, sizeof(base)) == 0, "w = 0 copies the base untouched, flags included");

    /* In place, which is how kiln_skel calls it. */
    T3DBone inplace[NB];
    memcpy(inplace, base, sizeof(base));
    kiln_pose_blend_masked(inplace, inplace, over, NB, mask, 1.0f);
    ok(NEAR(inplace[HAND_R].position.v[0], 3.0f) && NEAR(inplace[SHIN_L].position.v[0], 1.0f),
       "aliased out == base: hand follows the overlay, shin keeps the base");

    kiln_pose_blend_masked(out, base, over, NB, KILN_POSE_MASK_ALL, 1.0f);
    ok(NEAR(out[ROOT].position.v[0], 3.0f) && NEAR(out[SHIN_R].position.v[0], 3.0f),
       "KILN_POSE_MASK_ALL drives every bone");
}

int main(void)
{
    test_quat();
    test_mask();
    test_blend();
    if (g_fail) {
        printf("kiln-pose: %d FAILED\n", g_fail);
        return 1;
    }
    puts("kiln-pose: all passed");
    return 0;
}
