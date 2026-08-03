/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_clip.c — see m64_clip.h for the model.
 */

#include "m64_clip.h"

#include <libdragon.h> /* debugf, assertf */

/* Single-precision slab epsilon. At s16.16 world scale, positions are ~10^3;
 * 1e-3 (1 mm) silences the contact jitter that a tighter epsilon produces
 * without being perceptible to a player. See the header comment. */
#define CLIP_EPS (1e-3f)

static const M64Brush *g_brushes;
static uint16_t g_brush_count;

void m64_clip_set_world(const M64Brush *brushes, uint16_t count)
{
    g_brushes = brushes;
    g_brush_count = count;
}

/* Slab-method swept AABB vs AABB. Expands the static brush by the moving
 * box's extents (Minkowski sum), reducing the test to a ray-vs-AABB slab
 * test against the expanded box. Returns the entry t in [0,1] and the contact
 * normal on the axis that last raised tmin. */
static M64Trace sweep_box(fm_vec3_t start, fm_vec3_t end,
                          fm_vec3_t mins, fm_vec3_t maxs)
{
    M64Trace tr;
    tr.fraction = 1.0f;
    tr.endpos = end;
    tr.normal = (fm_vec3_t){ { 0, 0, 0 } };
    tr.hitsurface = 0;

    if (g_brush_count == 0) return tr;

    fm_vec3_t delta;
    fm_vec3_sub(&delta, &end, &start);

    float best_t = 1.0f;
    fm_vec3_t best_n = (fm_vec3_t){ { 0, 0, 0 } };
    uint8_t best_surf = 0;

    for (uint16_t i = 0; i < g_brush_count; i++) {
        const M64Brush *b = &g_brushes[i];

        /* Minkowski-expand the brush by the box: emins = b.mins - box.maxs,
         * emaxs = b.maxs - box.mins. The box's center then traces a ray
         * against [emins, emaxs]. */
        fm_vec3_t emins, emaxs;
        emins.v[0] = b->mins.v[0] - maxs.v[0];
        emins.v[1] = b->mins.v[1] - maxs.v[1];
        emins.v[2] = b->mins.v[2] - maxs.v[2];
        emaxs.v[0] = b->maxs.v[0] - mins.v[0];
        emaxs.v[1] = b->maxs.v[1] - mins.v[1];
        emaxs.v[2] = b->maxs.v[2] - mins.v[2];

        float tmin = 0.0f;
        float tmax = 1.0f;
        fm_vec3_t hit_n = (fm_vec3_t){ { 0, 0, 0 } };
        int axis_decided = 0; /* which axis last raised tmin */

        for (int a = 0; a < 3; a++) {
            if (delta.v[a] > -CLIP_EPS && delta.v[a] < CLIP_EPS) {
                /* Parallel to this slab. If the start center is outside the
                 * expanded slab on this axis, the box never overlaps the
                 * brush on this axis → no hit. */
                if (start.v[a] < emins.v[a] - CLIP_EPS ||
                    start.v[a] > emaxs.v[a] + CLIP_EPS) {
                    tmin = 2.0f; /* sentinel: no hit */
                    break;
                }
                continue;
            }
            float inv_d = 1.0f / delta.v[a];
            float t1 = (emins.v[a] - start.v[a]) * inv_d;
            float t2 = (emaxs.v[a] - start.v[a]) * inv_d;
            float n_sign;
            if (t1 > t2) {
                float tmp = t1; t1 = t2; t2 = tmp;
                n_sign = +1.0f; /* entered from the max side */
            } else {
                n_sign = -1.0f; /* entered from the min side */
            }
            if (t1 > tmin) {
                tmin = t1;
                hit_n = (fm_vec3_t){ { 0, 0, 0 } };
                hit_n.v[a] = n_sign;
                axis_decided = a;
            }
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) break; /* no overlap on this brush */
        }

        if (tmin > tmax) continue;       /* no overlap */
        if (tmin >= best_t) continue;    /* not the closest hit so far */
        if (tmin < -CLIP_EPS) continue;  /* already inside / behind; skip */

        best_t = tmin;
        best_n = hit_n;
        best_surf = b->surface;
        (void)axis_decided;
    }

    if (best_t < 1.0f) {
        if (best_t < 0.0f) best_t = 0.0f;
        tr.fraction = best_t;
        tr.normal = best_n;
        tr.hitsurface = best_surf;
        /* endpos = start + delta * fraction */
        tr.endpos.v[0] = start.v[0] + delta.v[0] * best_t;
        tr.endpos.v[1] = start.v[1] + delta.v[1] * best_t;
        tr.endpos.v[2] = start.v[2] + delta.v[2] * best_t;
    }
    return tr;
}

M64Trace m64_clip_box(fm_vec3_t start, fm_vec3_t end,
                      fm_vec3_t mins, fm_vec3_t maxs)
{
    return sweep_box(start, end, mins, maxs);
}

M64Trace m64_clip_ray(fm_vec3_t start, fm_vec3_t end)
{
    fm_vec3_t zero = (fm_vec3_t){ { 0, 0, 0 } };
    return sweep_box(start, end, zero, zero);
}

fm_vec3_t m64_clip_slide(fm_vec3_t pos, fm_vec3_t vel,
                         fm_vec3_t mins, fm_vec3_t maxs,
                         int max_iter)
{
    if (max_iter < 1) max_iter = 1;

    fm_vec3_t cur = pos;
    fm_vec3_t remaining = vel;

    for (int it = 0; it < max_iter; it++) {
        /* If the remaining displacement is near-zero, we're done — avoid a
         * divide-by-zero in the slab test's `1/delta` per axis. */
        float r2 = remaining.v[0] * remaining.v[0]
                 + remaining.v[1] * remaining.v[1]
                 + remaining.v[2] * remaining.v[2];
        if (r2 < CLIP_EPS * CLIP_EPS) break;

        fm_vec3_t end;
        end.v[0] = cur.v[0] + remaining.v[0];
        end.v[1] = cur.v[1] + remaining.v[1];
        end.v[2] = cur.v[2] + remaining.v[2];

        M64Trace tr = sweep_box(cur, end, mins, maxs);
        cur = tr.endpos;

        if (tr.fraction >= 1.0f) break; /* clean full move */

        /* Clip remaining velocity along the contact normal: remove the
         * component going into the wall. v -= (v·n) * n. The 1e-3 nudge
         * along the normal keeps us off the contact plane so the next trace
         * doesn't re-hit the same brush at fraction 0. */
        float vn = remaining.v[0] * tr.normal.v[0]
                 + remaining.v[1] * tr.normal.v[1]
                 + remaining.v[2] * tr.normal.v[2];
        remaining.v[0] -= vn * tr.normal.v[0];
        remaining.v[1] -= vn * tr.normal.v[1];
        remaining.v[2] -= vn * tr.normal.v[2];

        /* Scale remaining by the unused fraction of this trace, since `cur`
         * only advanced by `tr.fraction` of the displacement we traced. */
        float rest = 1.0f - tr.fraction;
        remaining.v[0] *= rest;
        remaining.v[1] *= rest;
        remaining.v[2] *= rest;

        /* Nudge out along the normal to avoid re-contacting the same face. */
        cur.v[0] += tr.normal.v[0] * CLIP_EPS;
        cur.v[1] += tr.normal.v[1] * CLIP_EPS;
        cur.v[2] += tr.normal.v[2] * CLIP_EPS;
    }
    return cur;
}

M64Trace m64_clip_ground(fm_vec3_t pos, fm_vec3_t mins, fm_vec3_t maxs)
{
    fm_vec3_t down = (fm_vec3_t){ {
        pos.v[0], pos.v[1] - 2.0f, pos.v[2],
    } };
    return sweep_box(pos, down, mins, maxs);
}