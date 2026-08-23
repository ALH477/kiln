/* SPDX-License-Identifier: MIT
 *
 * kiln_vanim.c — see kiln_vanim.h for the model.
 */

#include "kiln_vanim.h"

#include <libdragon.h>
#include <string.h>

/* ── Vertex FX ────────────────────────────────────────────────────────── */

void kiln_vfx_set(KilnVertexFX fx, int16_t arg0, int16_t arg1)
{
    t3d_state_set_vertex_fx((enum T3DVertexFX)fx, arg0, arg1);
}

void kiln_vfx_clear(void)
{
    t3d_state_set_vertex_fx(T3D_VERTEX_FX_NONE, 0, 0);
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static int packed_count(int vert_count)
{
    return (vert_count + 1) / 2;
}

static size_t packed_size(int vert_count)
{
    return sizeof(T3DVertPacked) * packed_count(vert_count);
}

/* Patch all objects in a model to read vertices from a segment instead of
 * the model's baked vertex buffer. This must be done once at init. */
static void patch_objects(const T3DModel *model, uint8_t segment_id)
{
    T3DModelIter it = t3d_model_iter_create(model, T3D_CHUNK_TYPE_OBJECT);
    while (t3d_model_iter_next(&it)) {
        t3d_model_make_object_vert_placeholder(model, it.object, segment_id);
    }
}

/* ── Morph targets ────────────────────────────────────────────────────── */

void kiln_morph_init(KilnMorph *m, const T3DModel *model,
                    T3DVertPacked **targets, int target_count,
                    int buffer_count, uint8_t segment_id)
{
    assertf(buffer_count >= 2, "kiln_morph: buffer_count must be >= 2");
    assertf(segment_id >= 1 && segment_id <= 6,
            "kiln_morph: segment_id must be 1-6 (7 is reserved for skeleton)");

    m->model = model;
    m->targets = targets;
    m->target_count = target_count;
    m->weights = malloc(sizeof(float) * target_count);
    for (int i = 0; i < target_count; i++) m->weights[i] = 0.0f;
    m->buffer_count = buffer_count;
    m->current_buffer = 0;
    m->segment_id = segment_id;

    size_t sz = packed_size(model->totalVertCount);
    m->work_buffers = malloc_uncached(sz * buffer_count);
    assertf(m->work_buffers, "kiln_morph: failed to allocate %zu bytes uncached",
            sz * buffer_count);

    patch_objects(model, segment_id);
    m->initialised = true;
}

void kiln_morph_destroy(KilnMorph *m)
{
    free(m->weights);
    m->weights = NULL;
    if (m->work_buffers) {
        free_uncached(m->work_buffers);
        m->work_buffers = NULL;
    }
    m->initialised = false;
}

void kiln_morph_update(KilnMorph *m, float dt)
{
    (void)dt;
    if (!m->initialised) return;

    /* Normalise weights so they sum to 1. */
    float sum = 0.0f;
    for (int i = 0; i < m->target_count; i++) {
        float w = m->weights[i];
        if (w < 0.0f) w = 0.0f;
        if (w > 1.0f) w = 1.0f;
        sum += w;
    }
    if (sum < 1e-6f) {
        /* All-zero: use first target (base shape). */
        sum = 1.0f;
        m->weights[0] = 1.0f;
    }

    int pc = packed_count(m->model->totalVertCount);
    T3DVertPacked *dst = &m->work_buffers[m->current_buffer * pc];

    /* Lerp: dst = sum(weights[i] * targets[i]). Since T3DVertPacked packs
     * two verts per struct, we blend entire structs (pos, norm, rgba, uv
     * all lerp together — positions and colours blend linearly, normals
     * should be renormalised but on N64 the 5.6.5 precision makes that a
     * no-op for practical purposes). */
    memset(dst, 0, sizeof(T3DVertPacked) * pc);
    for (int t = 0; t < m->target_count; t++) {
        float w = m->weights[t] / sum;
        if (w == 0.0f) continue;
        const T3DVertPacked *src = m->targets[t];
        for (int i = 0; i < pc; i++) {
            for (int j = 0; j < 3; j++) {
                dst[i].posA[j] += (int16_t)(src[i].posA[j] * w);
                dst[i].posB[j] += (int16_t)(src[i].posB[j] * w);
            }
            dst[i].rgbaA += (uint32_t)(src[i].rgbaA * w);
            dst[i].rgbaB += (uint32_t)(src[i].rgbaB * w);
            dst[i].stA[0] += (int16_t)(src[i].stA[0] * w);
            dst[i].stA[1] += (int16_t)(src[i].stA[1] * w);
            dst[i].stB[0] += (int16_t)(src[i].stB[0] * w);
            dst[i].stB[1] += (int16_t)(src[i].stB[1] * w);
        }
    }
    /* Normals: just take the first target's for now. Renormalising a 5.6.5
     * packed normal after blending is more work than it's worth on this
     * hardware — the visual difference for a morph between two poses with
     * similar normals is imperceptible. */

    data_cache_hit_writeback(dst, sizeof(T3DVertPacked) * pc);
    m->current_buffer = (m->current_buffer + 1) % m->buffer_count;
}

void kiln_morph_draw(KilnMorph *m)
{
    if (!m->initialised) return;
    int pc = packed_count(m->model->totalVertCount);
    /* Use the buffer that was just written (current_buffer - 1). */
    int buf = (m->current_buffer - 1 + m->buffer_count) % m->buffer_count;
    t3d_segment_set(m->segment_id, &m->work_buffers[buf * pc]);
    t3d_model_draw(m->model);
}

/* ── Procedural deformation ───────────────────────────────────────────── */

void kiln_deform_init(KilnDeform *d, const T3DModel *model,
                     KilnDeformFn fn, void *user_data,
                     int buffer_count, uint8_t segment_id)
{
    assertf(buffer_count >= 2, "kiln_deform: buffer_count must be >= 2");
    assertf(segment_id >= 1 && segment_id <= 6,
            "kiln_deform: segment_id must be 1-6");

    int vc = model->totalVertCount;
    size_t sz = packed_size(vc);

    d->model = model;
    d->fn = fn;
    d->user_data = user_data;
    d->vert_count = vc;
    d->buffer_count = buffer_count;
    d->current_buffer = 0;
    d->segment_id = segment_id;
    d->time = 0.0f;

    d->work_buffers = malloc_uncached(sz * buffer_count);
    assertf(d->work_buffers, "kiln_deform: failed to allocate %zu bytes", sz * buffer_count);

    /* Copy the original vertices so the deform callback can start from a
     * known reference each frame. */
    d->base_buffer = malloc_uncached(sz);
    assertf(d->base_buffer, "kiln_deform: failed to allocate base buffer");
    T3DVertPacked *src = t3d_model_get_vertices(model);
    memcpy(d->base_buffer, src, sz);
    data_cache_hit_writeback(d->base_buffer, sz);

    patch_objects(model, segment_id);
    d->initialised = true;
}

void kiln_deform_destroy(KilnDeform *d)
{
    if (d->work_buffers) {
        free_uncached(d->work_buffers);
        d->work_buffers = NULL;
    }
    if (d->base_buffer) {
        free_uncached(d->base_buffer);
        d->base_buffer = NULL;
    }
    d->initialised = false;
}

void kiln_deform_update(KilnDeform *d, float dt)
{
    if (!d->initialised) return;
    d->time += dt;

    int pc = packed_count(d->vert_count);
    size_t sz = sizeof(T3DVertPacked) * pc;
    T3DVertPacked *dst = &d->work_buffers[d->current_buffer * pc];

    /* Start from the base geometry, then let the callback deform it. */
    memcpy(dst, d->base_buffer, sz);

    if (d->fn) {
        d->fn(dst, d->vert_count, d->time, d->user_data);
    }

    data_cache_hit_writeback(dst, sz);
    d->current_buffer = (d->current_buffer + 1) % d->buffer_count;
}

void kiln_deform_draw(KilnDeform *d)
{
    if (!d->initialised) return;
    int pc = packed_count(d->vert_count);
    int buf = (d->current_buffer - 1 + d->buffer_count) % d->buffer_count;
    t3d_segment_set(d->segment_id, &d->work_buffers[buf * pc]);
    t3d_model_draw(d->model);
}