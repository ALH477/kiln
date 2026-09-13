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

static float clamp01(float w)
{
    return w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
}

/* Round to nearest, no libm. */
static int16_t round_i16(float x)
{
    return (int16_t)(x < 0.0f ? x - 0.5f : x + 0.5f);
}

static uint8_t round_u8(float x)
{
    if (x <= 0.0f) return 0;
    if (x >= 255.0f) return 255;
    return (uint8_t)(x + 0.5f);
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

/* t3d_vert_pack_normal's 5.6.5 fields are SIGNED, scaled by 15.5/31.5/15.5
 * (CLAUDE.md's .t3dm traps). Unpacked, lerped, repacked. Two identical
 * normals stay bit-identical, and two that cancel keep the first. */
static void unpack_normal(uint16_t n, T3DVec3 *out)
{
    int x = (n >> 11) & 0x1F, y = (n >> 5) & 0x3F, z = n & 0x1F;
    if (x & 0x10) x -= 32;
    if (y & 0x20) y -= 64;
    if (z & 0x10) z -= 32;
    out->v[0] = (float)x / 15.5f;
    out->v[1] = (float)y / 31.5f;
    out->v[2] = (float)z / 15.5f;
}

static uint16_t blend_normal(uint16_t a, uint16_t b, float t)
{
    if (a == b) return a;
    T3DVec3 na, nb, n;
    unpack_normal(a, &na);
    unpack_normal(b, &nb);
    for (int k = 0; k < 3; k++) n.v[k] = na.v[k] + (nb.v[k] - na.v[k]) * t;
    /* Shorter than 0.1 is two normals cancelling. Not zero, because the
     * packing is asymmetric (+Y is 31/31.5, -Y is -32/31.5): +Y and -Y at a
     * half each leave -0.016 of Y, which would otherwise repack as -Y. */
    if (n.v[0] * n.v[0] + n.v[1] * n.v[1] + n.v[2] * n.v[2] < 1e-2f) return a;
    return t3d_vert_pack_normal(&n);
}

void kiln_morph_update(KilnMorph *m, float dt)
{
    (void)dt;
    if (!m->initialised) return;

    /* Normalise the CLAMPED weights. The sum used to be taken over clamped
     * weights while the blend divided the raw ones, so a weight of 3 pushed
     * the shape three times past its target. */
    float sum = 0.0f;
    int dominant = 0, second = -1;
    float best = -1.0f, next = -1.0f;
    for (int t = 0; t < m->target_count; t++) {
        const float w = clamp01(m->weights[t]);
        sum += w;
        if (w > best) {
            next = best; second = dominant >= 0 && best >= 0.0f ? dominant : -1;
            best = w; dominant = t;
        } else if (w > next) {
            next = w; second = t;
        }
    }
    const int all_zero = sum < 1e-6f;   /* then the base shape, target 0 */

    int pc = packed_count(m->model->totalVertCount);
    T3DVertPacked *dst = &m->work_buffers[m->current_buffer * pc];
    const T3DVertPacked *dom = m->targets[all_zero ? 0 : dominant];
    /* Normals blend between the two heaviest targets once the lighter one has
     * a real share. Below that it is the dominant target's normal exactly, as
     * it always was — which is also every frame of a morph that is HOLDING a
     * shape, where re-quantising would be work for nothing. */
    const float share = (!all_zero && second >= 0 && next > 0.0f) ? next / (best + next) : 0.0f;
    const T3DVertPacked *sec = share >= 0.05f ? m->targets[second] : NULL;

    /* dst = sum(w[t] * targets[t]), accumulated in float per vertex and
     * rounded ONCE. Truncating each target's share separately lost up to one
     * unit per target — three identical targets came out three units short.
     *
     * Colour is RGBA8, four 8-bit channels in one uint32, so it is blended
     * per channel. It used to scale the packed word by a float, which carries
     * between channels: red + green at a half each came out (127,255,128).
     *
     * Normals: a sum of 5.6.5 packed normals is not a normal, so they are
     * unpacked, blended between the two heaviest targets, and repacked (which
     * normalises). They used to come from the most-weighted target alone, and
     * the lighting jumped at the 50% crossover in the middle of every morph.
     * Before that they were left at the memset's zero — which lights nothing. */
    for (int i = 0; i < pc; i++) {
        float pa[3] = { 0 }, pb[3] = { 0 }, ca[4] = { 0 }, cb[4] = { 0 };
        float sa[2] = { 0 }, sb[2] = { 0 };
        for (int t = 0; t < m->target_count; t++) {
            const float w = all_zero ? (t == 0 ? 1.0f : 0.0f)
                                     : clamp01(m->weights[t]) / sum;
            if (w == 0.0f) continue;
            const T3DVertPacked *src = &m->targets[t][i];
            for (int j = 0; j < 3; j++) {
                pa[j] += src->posA[j] * w;
                pb[j] += src->posB[j] * w;
            }
            for (int c = 0; c < 4; c++) {
                ca[c] += (float)((src->rgbaA >> (24 - 8 * c)) & 0xFF) * w;
                cb[c] += (float)((src->rgbaB >> (24 - 8 * c)) & 0xFF) * w;
            }
            for (int j = 0; j < 2; j++) {
                sa[j] += src->stA[j] * w;
                sb[j] += src->stB[j] * w;
            }
        }
        for (int j = 0; j < 3; j++) {
            dst[i].posA[j] = round_i16(pa[j]);
            dst[i].posB[j] = round_i16(pb[j]);
        }
        uint32_t rgba_a = 0, rgba_b = 0;
        for (int c = 0; c < 4; c++) {
            rgba_a |= (uint32_t)round_u8(ca[c]) << (24 - 8 * c);
            rgba_b |= (uint32_t)round_u8(cb[c]) << (24 - 8 * c);
        }
        dst[i].rgbaA = rgba_a;
        dst[i].rgbaB = rgba_b;
        for (int j = 0; j < 2; j++) {
            dst[i].stA[j] = round_i16(sa[j]);
            dst[i].stB[j] = round_i16(sb[j]);
        }
        dst[i].normA = sec ? blend_normal(dom[i].normA, sec[i].normA, share) : dom[i].normA;
        dst[i].normB = sec ? blend_normal(dom[i].normB, sec[i].normB, share) : dom[i].normB;
    }

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