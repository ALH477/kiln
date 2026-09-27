/* SPDX-License-Identifier: MIT
 *
 * kiln_cull.c — see kiln_cull.h.
 */
#include "kiln_cull.h"

#include <fmath.h>
#include <math.h>
#include <stddef.h>

uint8_t fig_cull_tier_of(const char *name)
{
    if (!name) return 0;

    /* Find the last '.' without strrchr, so this unit pulls in no string
     * header on either target. */
    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }
    /* A leading dot has no base name before it, so `.lod1` is a name, not a
     * tier — the same reason `lod1` alone is not one. */
    if (!dot || dot == name) return 0;

    const char *s = dot + 1;
    if (s[0] != 'l' || s[1] != 'o' || s[2] != 'd') return 0;
    s += 3;
    if (*s < '0' || *s > '9') return 0;   /* bare ".lod", or ".lodX" */

    unsigned n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;   /* ".lod2x" is a name */
        n = n * 10u + (unsigned)(*s - '0');
        if (n > 255u) return 255u;            /* saturate rather than wrap */
    }
    return (uint8_t)n;
}

void fig_cull_sphere_of(const int16_t aabb_min[3], const int16_t aabb_max[3],
                         const float pos[3], float scale, float yaw,
                         float out_centre[3], float *out_radius)
{
    /* Midpoint and half-extent in model space. The AABB is in the same integer
     * units as the vertices it bounds, which is the space the model matrix
     * transforms, so the placement below is the same transform the RSP will
     * apply to the geometry. */
    const float cx = ((float)aabb_min[0] + (float)aabb_max[0]) * 0.5f;
    const float cy = ((float)aabb_min[1] + (float)aabb_max[1]) * 0.5f;
    const float cz = ((float)aabb_min[2] + (float)aabb_max[2]) * 0.5f;

    const float hx = ((float)aabb_max[0] - (float)aabb_min[0]) * 0.5f;
    const float hy = ((float)aabb_max[1] - (float)aabb_min[1]) * 0.5f;
    const float hz = ((float)aabb_max[2] - (float)aabb_min[2]) * 0.5f;

    /* Scale first, then rotate, then translate — S, R, T in that order, which
     * is what FigTransform's matrix does. */
    const float sx = cx * scale, sy = cy * scale, sz = cz * scale;

    const float c = fm_cosf(yaw), s = fm_sinf(yaw);
    out_centre[0] = pos[0] + (sx * c + sz * s);
    out_centre[1] = pos[1] + sy;
    out_centre[2] = pos[2] + (sz * c - sx * s);

    /* The radius needs no rotation: a sphere is the one bound that does not
     * care which way the box is turned. It does need the scale, and a uniform
     * scale multiplies it exactly. */
    /* sqrtf, not fm_sqrtf: fmath does not have one, because plain sqrtf
     * already compiles to the VR4300's sqrt.s opcode — the same note
     * kiln_crater.c carries. */
    *out_radius = sqrtf(hx * hx + hy * hy + hz * hz) * scale;
}

float fig_cull_surface_dist_sq(const int16_t aabb_min[3],
                                const int16_t aabb_max[3],
                                const float pos[3], float scale, float yaw,
                                const float eye[3])
{
    float centre[3], radius;
    fig_cull_sphere_of(aabb_min, aabb_max, pos, scale, yaw, centre, &radius);

    const float dx = centre[0] - eye[0];
    const float dy = centre[1] - eye[1];
    const float dz = centre[2] - eye[2];
    const float d2 = dx * dx + dy * dy + dz * dz;

    /* Compare squared against squared rather than taking a root to subtract
     * the radius: inside the sphere there is no distance to the surface worth
     * reporting, and outside it the root is needed exactly once. */
    const float r2 = radius * radius;
    if (d2 <= r2) return 0.0f;

    const float d = sqrtf(d2) - radius;
    return d * d;
}
