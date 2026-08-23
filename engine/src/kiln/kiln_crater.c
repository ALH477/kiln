/* SPDX-License-Identifier: MIT
 *
 * kiln_crater.c — see kiln_crater.h.
 */
#include "kiln_crater.h"

#include <libdragon.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <t3d/t3d.h>

/* Concrete lifecycle, not "some decay": punch ramps 0 -> depth_max over
 * IMPACT_PUNCH_TIME, then eases depth_max -> 0 over HEAL_TIME. Both stages
 * use the same smoothstep, which is point-symmetric (smoothstep(1-t) ==
 * 1-smoothstep(t)) so the heal is exactly the punch curve run backwards. */
#define KILN_CRATER_PUNCH_TIME 0.15f
#define KILN_CRATER_HEAL_TIME  7.5f

static inline float smoothstep01(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* Current sink this slot contributes at local (x, z): its own punch/heal
 * depth times a radial falloff to 0 at `radius`. Shared by
 * kiln_crater_update (per vertex) and kiln_crater_sample (per query point)
 * so the two can never disagree about what "the current crater shape" is. */
static float crater_contribution(const KilnCraterSlot *s, float x, float z)
{
    float depth_now;
    if (s->age < KILN_CRATER_PUNCH_TIME) {
        depth_now = s->depth_max * smoothstep01(s->age / KILN_CRATER_PUNCH_TIME);
    } else {
        float t_heal = (s->age - KILN_CRATER_PUNCH_TIME) / KILN_CRATER_HEAL_TIME;
        depth_now = s->depth_max * (1.0f - smoothstep01(t_heal));
    }
    if (depth_now <= 0.0f) return 0.0f;

    const float dx = x - s->x, dz = z - s->z;
    /* sqrtf, not fm_sqrtf: fmath has no fm_sqrtf because plain sqrtf
     * already compiles to the sqrt.s opcode — this is one sqrt per active
     * crater per vertex within its radius, not a hot inner loop run every
     * frame regardless. */
    const float dist = sqrtf(dx * dx + dz * dz);
    const float falloff = 1.0f - smoothstep01(dist / s->radius);
    return depth_now * falloff;
}

int kiln_crater_init(KilnCraterField *cf, const T3DModel *model,
                    const char *object_name)
{
    memset(cf, 0, sizeof(*cf));
    cf->model = model;

    T3DObject *obj = t3d_model_get_object(model, object_name);
    if (!obj) {
        debugf("kiln_crater: object '%s' not found; craters disabled\n",
               object_name);
        return -1;
    }
    cf->object = obj;

    int total = 0;
    for (int p = 0; p < obj->numParts; p++) {
        total += (int)obj->parts[p].vertLoadCount * 2;
    }
    if (total <= 0) {
        debugf("kiln_crater: object '%s' has no vertices; craters disabled\n",
               object_name);
        cf->object = NULL;
        return -1;
    }

    cf->verts = malloc(sizeof(KilnCraterVert) * (size_t)total);
    if (!cf->verts) {
        debugf("kiln_crater: no memory for %d vertices; craters disabled\n",
               total);
        cf->object = NULL;
        return -1;
    }

    int vi = 0;
    for (int p = 0; p < obj->numParts; p++) {
        T3DObjectPart *part = &obj->parts[p];
        const int n = (int)part->vertLoadCount * 2;
        for (int i = 0; i < n; i++) {
            int16_t *pos = t3d_vertbuffer_get_pos(part->vert, i);
            cf->verts[vi].pos = pos;
            cf->verts[vi].x   = pos[0];
            cf->verts[vi].y0  = pos[1];
            cf->verts[vi].z   = pos[2];
            vi++;
        }
    }
    cf->vert_count = vi;
    return 0;
}

void kiln_crater_destroy(KilnCraterField *cf)
{
    if (cf->verts) {
        free(cf->verts);
        cf->verts = NULL;
    }
    cf->vert_count = 0;
    cf->object = NULL;
}

void kiln_crater_impact(KilnCraterField *cf, float x, float z,
                       float radius, float depth_max)
{
    if (cf->vert_count <= 0) return;

    int slot = -1;
    for (int i = 0; i < KILN_CRATER_MAX_ACTIVE; i++) {
        if (!cf->slots[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        /* All full: evict the OLDEST (highest age), not the newest — a
         * storm actively tearing up the ground reads better than a strike
         * that lands and is immediately dropped for lack of a slot. */
        float oldest = -1.0f;
        for (int i = 0; i < KILN_CRATER_MAX_ACTIVE; i++) {
            if (cf->slots[i].age > oldest) {
                oldest = cf->slots[i].age;
                slot = i;
            }
        }
    }

    cf->slots[slot].x = x;
    cf->slots[slot].z = z;
    cf->slots[slot].radius = radius;
    cf->slots[slot].depth_max = depth_max;
    cf->slots[slot].age = 0.0f;
    cf->slots[slot].active = 1;
}

void kiln_crater_update(KilnCraterField *cf, float dt)
{
    cf->touched_last_update = 0;

    int have_active = 0;
    for (int i = 0; i < KILN_CRATER_MAX_ACTIVE; i++) {
        if (cf->slots[i].active) {
            cf->slots[i].age += dt;
            have_active = 1;
        }
    }
    /* Zero cost the vast majority of frames: no crater has landed recently,
     * so there is nothing dirty to rewrite. */
    if (!have_active) return;

    for (int v = 0; v < cf->vert_count; v++) {
        KilnCraterVert *mv = &cf->verts[v];
        float sink = 0.0f;
        for (int i = 0; i < KILN_CRATER_MAX_ACTIVE; i++) {
            const KilnCraterSlot *s = &cf->slots[i];
            if (!s->active) continue;
            const float c = crater_contribution(s, (float)mv->x, (float)mv->z);
            /* MAX, not sum, across overlapping craters — two nearby
             * strikes should not dig a deeper pit than either alone. */
            if (c > sink) sink = c;
        }
        /* Absolute position from the untouched rest pose, not an
         * accumulated delta — see KilnCraterVert's comment. This also
         * correctly restores a vertex to its rest Y on the exact frame its
         * last covering crater finishes healing (sink naturally reaches 0
         * exactly then), with no separate "reset" path needed. */
        const int16_t new_y = (int16_t)((float)mv->y0 - sink);
        if (mv->pos[1] != new_y) {
            mv->pos[1] = new_y;
            cf->touched_last_update++;
        }
    }

    for (int i = 0; i < KILN_CRATER_MAX_ACTIVE; i++) {
        KilnCraterSlot *s = &cf->slots[i];
        if (s->active && s->age > KILN_CRATER_PUNCH_TIME + KILN_CRATER_HEAL_TIME) {
            s->active = 0;
        }
    }
}

float kiln_crater_sample(const KilnCraterField *cf, float x, float z)
{
    float sink = 0.0f;
    for (int i = 0; i < KILN_CRATER_MAX_ACTIVE; i++) {
        const KilnCraterSlot *s = &cf->slots[i];
        if (!s->active) continue;
        const float c = crater_contribution(s, x, z);
        if (c > sink) sink = c;
    }
    return sink;
}
