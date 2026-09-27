/* SPDX-License-Identifier: MIT
 *
 * kiln_detail.c — see kiln_detail.h.
 */
#include "kiln_detail.h"
#include "kiln_cull.h"

#include <stdio.h>

static FigDetailStats g_stats;

void fig_detail_frame_begin(void)
{
    g_stats = (FigDetailStats){ 0 };
}

const FigDetailStats *fig_detail_stats(void)
{
    return &g_stats;
}

/** Vertices an object actually loads: the sum of its parts' load counts.
 *  T3DObject has no vertex count of its own — the parts are the loads, and the
 *  load count is the number the RSP transforms. */
static uint16_t object_verts(const T3DObject *obj)
{
    uint16_t v = 0;
    for (uint16_t i = 0; i < obj->numParts; i++)
        v = (uint16_t)(v + obj->parts[i].vertLoadCount);
    return v;
}

int fig_detail_model_visible(const T3DModel *model, const T3DFrustum *frustum,
                              const FigDetailPlace *place)
{
    if (!model) return 0;
    if (!frustum || !place) return 1;

    T3DVec3 centre;
    float radius;
    fig_cull_sphere_of(model->aabbMin, model->aabbMax, place->pos,
                        place->scale, place->yaw, centre.v, &radius);
    if (!t3d_frustum_vs_sphere(frustum, &centre, radius)) {
        g_stats.models_culled++;
        return 0;
    }
    g_stats.models_drawn++;
    return 1;
}

uint8_t fig_detail_tier_available(const T3DModel *model, uint8_t want)
{
    if (!model) return 0;

    uint8_t best = 0;
    T3DModelIter it = t3d_model_iter_create(model, T3D_CHUNK_TYPE_OBJECT);
    while (t3d_model_iter_next(&it)) {
        const uint8_t t = fig_cull_tier_of(it.object->name);
        if (t <= want && t > best) best = t;
    }
    return best;
}

T3DObject *fig_detail_object(const T3DModel *model, const char *base,
                              uint8_t tier)
{
    if (!model || !base) return NULL;

    /* Walk down rather than up: the tier asked for may not exist, and the
     * next one that does is the right answer. Bounded by the tier, so this is
     * at most four name lookups and usually one. */
    for (int t = (int)tier; t > 0; t--) {
        /* Long enough for any object name gltf_to_t3d will have produced plus
         * a ".lodN" — and a name that would not fit is one that cannot match
         * anything, so truncation is not a correctness question here. */
        char name[48];
        const int n = snprintf(name, sizeof name, "%s.lod%d", base, t);
        if (n < 0 || (size_t)n >= sizeof name) continue;
        T3DObject *o = t3d_model_get_object(model, name);
        if (o) return o;
    }
    return t3d_model_get_object(model, base);
}

int fig_detail_admit(const T3DObject *obj, uint8_t tier,
                      const T3DFrustum *frustum,
                      const FigDetailPlace *place)
{
    if (!obj) return 0;

    if (fig_cull_tier_of(obj->name) != tier) {
        g_stats.skipped++;
        return 0;
    }

    if (frustum && place) {
        T3DVec3 centre;
        float radius;
        fig_cull_sphere_of(obj->aabbMin, obj->aabbMax, place->pos,
                            place->scale, place->yaw, centre.v, &radius);
        if (!t3d_frustum_vs_sphere(frustum, &centre, radius)) {
            /* isVisible is Tiny3D's own flag for exactly this verdict. Nothing
             * in the renderer reads it ("otherwise no effect on rendering",
             * t3dmodel.h:89), but a debug overlay can, and keeping it honest
             * costs one store. */
            ((T3DObject *)obj)->isVisible = 0;
            g_stats.culled++;
            return 0;
        }
    }

    ((T3DObject *)obj)->isVisible = 1;
    fig_detail_count(obj, tier);
    return 1;
}

void fig_detail_count(const T3DObject *obj, uint8_t tier)
{
    if (!obj) return;
    g_stats.drawn++;
    g_stats.tris = (uint16_t)(g_stats.tris + obj->triCount);
    g_stats.verts = (uint16_t)(g_stats.verts + object_verts(obj));
    g_stats.parts = (uint16_t)(g_stats.parts + obj->numParts);
    if (tier < 4) g_stats.tier_drawn[tier]++;
}

void fig_detail_draw(const T3DModel *model, const T3DMat4FP *bones,
                      uint8_t tier, const T3DFrustum *frustum,
                      const FigDetailPlace *place)
{
    if (!model) return;

    const uint8_t use = fig_detail_tier_available(model, tier);

    /* The state tracker is what keeps this from costing more than
     * t3d_model_draw_custom: without it every object would re-send a full
     * material even when the next one is identical. */
    T3DModelState state = t3d_model_state_create();
    T3DModelIter it = t3d_model_iter_create(model, T3D_CHUNK_TYPE_OBJECT);
    while (t3d_model_iter_next(&it)) {
        T3DObject *obj = it.object;
        if (!fig_detail_admit(obj, use, frustum, place)) continue;
        if (obj->material) t3d_model_draw_material(obj->material, &state);
        t3d_model_draw_object(obj, bones);
    }

    /* t3d_model_draw_custom's own epilogue (t3dmodel.c:291). Restated because
     * this loop replaces that function rather than calling it, and leaving a
     * vertex-FX function installed leaks it into whatever draws next. */
    if (state.lastVertFXFunc != T3D_VERTEX_FX_NONE)
        t3d_state_set_vertex_fx(T3D_VERTEX_FX_NONE, 0, 0);
}
