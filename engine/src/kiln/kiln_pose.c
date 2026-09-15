/* SPDX-License-Identifier: MIT
 *
 * kiln_pose.c — see kiln_pose.h for the model.
 */

#include "kiln_pose.h"

#include <math.h>

void kiln_quat_nlerp(T3DQuat *out, const T3DQuat *a, const T3DQuat *b, float t)
{
    /* Tiny3D's t3d_quat_nlerp (src/t3d/t3dmath.c), line for line: a negative
     * dot flips the weight on `a`, which is the short-path choice. */
    const float dot = a->v[0] * b->v[0] + a->v[1] * b->v[1] + a->v[2] * b->v[2] + a->v[3] * b->v[3];
    float blend = 1.0f - t;
    if (dot < 0.0f) blend = -blend;

    T3DQuat r;
    for (int i = 0; i < 4; i++) r.v[i] = blend * a->v[i] + t * b->v[i];

    const float len2 = r.v[0] * r.v[0] + r.v[1] * r.v[1] + r.v[2] * r.v[2] + r.v[3] * r.v[3];
    if (len2 > 0.0f) {
        const float inv = 1.0f / sqrtf(len2);
        for (int i = 0; i < 4; i++) r.v[i] *= inv;
    } else {
        r = (T3DQuat){{ 0.0f, 0.0f, 0.0f, 1.0f }};
    }
    *out = r;
}

void kiln_quat_mul(T3DQuat *out, const T3DQuat *a, const T3DQuat *b)
{
    const float ax = a->v[0], ay = a->v[1], az = a->v[2], aw = a->v[3];
    const float bx = b->v[0], by = b->v[1], bz = b->v[2], bw = b->v[3];
    out->v[0] = aw * bx + ax * bw + ay * bz - az * by;
    out->v[1] = aw * by - ax * bz + ay * bw + az * bx;
    out->v[2] = aw * bz + ax * by - ay * bx + az * bw;
    out->v[3] = aw * bw - ax * bx - ay * by - az * bz;
}

void kiln_quat_axis_angle(T3DQuat *out, float ax, float ay, float az, float angle)
{
    const float s = fm_sinf(angle * 0.5f);
    out->v[0] = ax * s;
    out->v[1] = ay * s;
    out->v[2] = az * s;
    out->v[3] = fm_cosf(angle * 0.5f);
}

void kiln_pose_blend_masked(T3DBone *out, const T3DBone *base, const T3DBone *over,
                            int n, uint32_t mask, float w)
{
    for (int i = 0; i < n; i++) {
        const int in = i < KILN_POSE_MAX_BONES ? (int)((mask >> i) & 1u)
                                               : mask == KILN_POSE_MASK_ALL;
        if (!in || w <= 0.0f) {
            if (out != base) out[i] = base[i];
            continue;
        }
        const T3DBone *b = &base[i], *o = &over[i];
        T3DQuat rot;
        kiln_quat_nlerp(&rot, &b->rotation, &o->rotation, w);
        for (int k = 0; k < 3; k++) {
            const float p = b->position.v[k] + (o->position.v[k] - b->position.v[k]) * w;
            const float s = b->scale.v[k] + (o->scale.v[k] - b->scale.v[k]) * w;
            out[i].position.v[k] = p;
            out[i].scale.v[k] = s;
        }
        out[i].rotation = rot;
        if (out != base) out[i].matrix = b->matrix;
        out[i].hasChanged = 1;
    }
}

uint32_t kiln_pose_subtree_mask(const uint16_t *depth, int n, int root)
{
    if (root < 0 || root >= n) return 0;
    uint32_t mask = 0;
    for (int j = root; j < n; j++) {
        if (j > root && depth[j] <= depth[root]) break;
        if (j < KILN_POSE_MAX_BONES) mask |= 1u << j;
    }
    return mask;
}
