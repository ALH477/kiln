/* SPDX-License-Identifier: MIT */

#include "kiln_sierp.h"

static void mid(fm_vec3_t *o, const fm_vec3_t *a, const fm_vec3_t *b)
{
    o->v[0] = 0.5f * (a->v[0] + b->v[0]);
    o->v[1] = 0.5f * (a->v[1] + b->v[1]);
    o->v[2] = 0.5f * (a->v[2] + b->v[2]);
}

static void child_at(KilnTet *o, const KilnTet *p, int i)
{
    o->v[0] = p->v[i];
    int k = 1;
    for (int j = 0; j < 4; j++) {
        if (j == i) continue;
        mid(&o->v[k], &p->v[i], &p->v[j]);
        k++;
    }
}

void kiln_sierp_regular(KilnTet *out, float radius)
{
    /* Corners of a regular tet. Each has length sqrt(3); scale to `radius`. */
    const float s = radius * (1.0f / 1.73205080757f);
    out->v[0] = (fm_vec3_t){{  s,  s,  s }};
    out->v[1] = (fm_vec3_t){{  s, -s, -s }};
    out->v[2] = (fm_vec3_t){{ -s,  s, -s }};
    out->v[3] = (fm_vec3_t){{ -s, -s,  s }};
}

void kiln_sierp_rotate_y(KilnTet *out, const KilnTet *in, float c, float s)
{
    for (int i = 0; i < 4; i++) {
        const float x = in->v[i].v[0], z = in->v[i].v[2];
        out->v[i].v[0] =  x * c + z * s;
        out->v[i].v[1] =  in->v[i].v[1];
        out->v[i].v[2] = -x * s + z * c;
    }
}

void kiln_sierp_negate(KilnTet *out, const KilnTet *in)
{
    for (int i = 0; i < 4; i++) {
        out->v[i].v[0] = -in->v[i].v[0];
        out->v[i].v[1] = -in->v[i].v[1];
        out->v[i].v[2] = -in->v[i].v[2];
    }
}

int kiln_sierp_leaves(KilnTet *out, int cap, const KilnTet *root, int depth)
{
    if (!out || !root || cap < 1) return 0;
    if (depth < 0) depth = 0;
    if (depth > KILN_SIERP_MAX_DEPTH) depth = KILN_SIERP_MAX_DEPTH;

    out[0] = *root;
    int n = 1;
    for (int d = 0; d < depth; d++) {
        if (n * 4 > cap) return n;
        for (int i = n - 1; i >= 0; i--) {
            const KilnTet p = out[i];
            child_at(&out[i * 4 + 0], &p, 0);
            child_at(&out[i * 4 + 1], &p, 1);
            child_at(&out[i * 4 + 2], &p, 2);
            child_at(&out[i * 4 + 3], &p, 3);
        }
        n *= 4;
    }
    return n;
}

void kiln_sierp_morph(KilnTet *dst, const KilnTet *a, const KilnTet *b,
                      int n, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float u = 1.0f - t;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 4; k++) {
            dst[i].v[k].v[0] = a[i].v[k].v[0] * u + b[i].v[k].v[0] * t;
            dst[i].v[k].v[1] = a[i].v[k].v[1] * u + b[i].v[k].v[1] * t;
            dst[i].v[k].v[2] = a[i].v[k].v[2] * u + b[i].v[k].v[2] * t;
        }
    }
}

float kiln_sierp_smooth(float t)
{
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}
