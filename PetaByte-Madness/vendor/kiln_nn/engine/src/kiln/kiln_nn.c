/* SPDX-License-Identifier: MPL-2.0 */
/**
 * kiln_nn.c — quantized inference kernels for N64 NPC policies.
 *
 * Production path is int8 weights + int32 accumulators.
 * Written so a host reference can be bit-exact with the console binary.
 *
 * No malloc. No libm on the integer path.
 */

#include "kiln_nn.h"
#include <string.h>

/* ── Internal helpers ─────────────────────────────────────────────────── */

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int8_t clamp_i8(int32_t v)
{
    return (int8_t)clamp_i32(v, -128, 127);
}

static inline float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ── Scratch sizing ───────────────────────────────────────────────────── */
/*
 * Two alternating activation buffers (A/B) of the widest layer.
 * We size for the worst case (int32 or float).
 */
size_t kiln_nn_scratch_bytes(uint16_t max_width, uint8_t n_layers)
{
    (void)n_layers;
    /* 2 * max_width * sizeof(int32_t) + alignment headroom */
    return ((size_t)max_width * 2u * sizeof(int32_t)) + 64u;
}

/* ── Init ─────────────────────────────────────────────────────────────── */

bool kiln_nn_init(KilnNnModel *m,
                 KilnNnDtype dtype,
                 uint8_t n_layers,
                 const KilnNnLayer *layers,
                 uint16_t obs_dim,
                 uint16_t action_dim,
                 const int16_t *tanh_lut,
                 void *scratch,
                 size_t scratch_bytes)
{
    if (!m || !layers || n_layers == 0 || n_layers > KILN_NN_MAX_LAYERS)
        return false;
    if (obs_dim == 0 || obs_dim > KILN_NN_MAX_OBS)
        return false;
    if (action_dim == 0 || action_dim > KILN_NN_MAX_ACTIONS)
        return false;

    uint16_t max_w = obs_dim;
    for (uint8_t i = 0; i < n_layers; i++) {
        if (layers[i].in_features  > KILN_NN_MAX_WIDTH ||
            layers[i].out_features > KILN_NN_MAX_WIDTH)
            return false;
        if (layers[i].out_features > max_w)
            max_w = layers[i].out_features;
        if (i == 0 && layers[i].in_features != obs_dim)
            return false;
        if (i + 1 == n_layers && layers[i].out_features != action_dim)
            return false;
    }

    size_t need = kiln_nn_scratch_bytes(max_w, n_layers);
    if (!scratch || scratch_bytes < need)
        return false;

    memset(m, 0, sizeof(*m));
    m->dtype         = dtype;
    m->n_layers      = n_layers;
    m->obs_dim       = obs_dim;
    m->action_dim    = action_dim;
    m->tanh_lut      = tanh_lut;
    m->scratch       = scratch;
    m->scratch_bytes = scratch_bytes;

    for (uint8_t i = 0; i < n_layers; i++)
        m->layers[i] = layers[i];

    return true;
}

/* ── Integer dense + activation ───────────────────────────────────────── */

static void dense_i8(const KilnNnLayer *L, const int8_t *in, int32_t *out)
{
    const int in_f  = (int)L->in_features;
    const int out_f = (int)L->out_features;
    const int8_t  *w = L->weights_i8;
    const int32_t *b = L->bias_i32;
    const int shift  = L->scale_shift;

    for (int o = 0; o < out_f; o++) {
        int32_t acc = b ? b[o] : 0;
        const int8_t *row = w + (size_t)o * (size_t)in_f;
        for (int i = 0; i < in_f; i++)
            acc += (int32_t)row[i] * (int32_t)in[i];

        if (shift > 0)
            acc >>= shift;
        else if (shift < 0)
            acc <<= (-shift);

        switch (L->act) {
        case KILN_NN_ACT_RELU:
            if (acc < 0) acc = 0;
            break;
        case KILN_NN_ACT_HARDTANH:
            acc = clamp_i32(acc, -128, 127);
            break;
        case KILN_NN_ACT_TANH_LUT:
            /* Fallback to hardtanh when no LUT is supplied.
             * Real LUT application is left to a future helper. */
            acc = clamp_i32(acc, -128, 127);
            break;
        case KILN_NN_ACT_LINEAR:
        default:
            break;
        }
        out[o] = acc;
    }
}

static void quantize_act_i8(const int32_t *src, int8_t *dst, int n)
{
    for (int i = 0; i < n; i++)
        dst[i] = clamp_i8(src[i]);
}

/* ── Float dense + activation ─────────────────────────────────────────── */

static void dense_f32(const KilnNnLayer *L, const float *in, float *out)
{
    const int in_f  = (int)L->in_features;
    const int out_f = (int)L->out_features;
    const float *w  = L->weights_f32;
    const float *b  = L->bias_f32;

    for (int o = 0; o < out_f; o++) {
        float acc = b ? b[o] : 0.f;
        const float *row = w + (size_t)o * (size_t)in_f;
        for (int i = 0; i < in_f; i++)
            acc += row[i] * in[i];

        switch (L->act) {
        case KILN_NN_ACT_RELU:
            if (acc < 0.f) acc = 0.f;
            break;
        case KILN_NN_ACT_HARDTANH:
            acc = clampf(acc, -1.f, 1.f);
            break;
        case KILN_NN_ACT_TANH_LUT: {
            /* Cheap Padé approximation — no libm required */
            float x  = clampf(acc, -3.f, 3.f);
            float x2 = x * x;
            acc = x * (27.f + x2) / (27.f + 9.f * x2);
            break;
        }
        case KILN_NN_ACT_LINEAR:
        default:
            break;
        }
        out[o] = acc;
    }
}

/* ── Forward ──────────────────────────────────────────────────────────── */

bool kiln_nn_forward(const KilnNnModel *m, const void *obs, void *logits)
{
    if (!m || !obs || !logits || !m->scratch)
        return false;

    uint16_t max_w = m->obs_dim;
    for (uint8_t i = 0; i < m->n_layers; i++)
        if (m->layers[i].out_features > max_w)
            max_w = m->layers[i].out_features;

    if (m->dtype == KILN_NN_DTYPE_INT8) {
        /* Layout:
         *   scratch[0 .. max_w)           → int8 activation / observation
         *   scratch[max_w .. 2*max_w)     → int32 accumulators
         */
        int8_t  *act  = (int8_t  *)m->scratch;
        int32_t *acc  = (int32_t *)((uint8_t *)m->scratch +
                                    (size_t)max_w * sizeof(int32_t));

        memcpy(act, obs, (size_t)m->obs_dim * sizeof(int8_t));

        for (uint8_t li = 0; li < m->n_layers; li++) {
            const KilnNnLayer *L = &m->layers[li];
            dense_i8(L, act, acc);

            if (li + 1 < m->n_layers) {
                quantize_act_i8(acc, act, (int)L->out_features);
            }
        }

        memcpy(logits, acc, (size_t)m->action_dim * sizeof(int32_t));
        return true;
    }

    /* Float path */
    {
        float *buf_a = (float *)m->scratch;
        float *buf_b = buf_a + max_w;

        memcpy(buf_a, obs, (size_t)m->obs_dim * sizeof(float));

        const float *cur_in  = buf_a;
        float       *cur_out = buf_b;

        for (uint8_t li = 0; li < m->n_layers; li++) {
            const KilnNnLayer *L = &m->layers[li];
            dense_f32(L, cur_in, cur_out);

            if (li + 1 < m->n_layers) {
                const float *tmp = cur_in;
                cur_in  = cur_out;
                cur_out = (float *)tmp;
            }
        }

        memcpy(logits, cur_out, (size_t)m->action_dim * sizeof(float));
        return true;
    }
}

/* ── Argmax ───────────────────────────────────────────────────────────── */

int kiln_nn_argmax(const KilnNnModel *m, const void *obs)
{
    if (!m) return -1;

    if (m->dtype == KILN_NN_DTYPE_INT8) {
        int32_t logits[KILN_NN_MAX_ACTIONS];
        if (!kiln_nn_forward(m, obs, logits))
            return -1;
        int best = 0;
        int32_t best_v = logits[0];
        for (int i = 1; i < (int)m->action_dim; i++) {
            if (logits[i] > best_v) {
                best_v = logits[i];
                best   = i;
            }
        }
        return best;
    }

    float logits[KILN_NN_MAX_ACTIONS];
    if (!kiln_nn_forward(m, obs, logits))
        return -1;
    int best = 0;
    float best_v = logits[0];
    for (int i = 1; i < (int)m->action_dim; i++) {
        if (logits[i] > best_v) {
            best_v = logits[i];
            best   = i;
        }
    }
    return best;
}

/* ── Optional sampling (float path only) ──────────────────────────────── */
/*
 * Production NPCs should prefer kiln_nn_argmax.
 * This helper is provided for experiments; it uses a soft-max approximation
 * that avoids libm (no expf).
 */

static uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

int kiln_nn_sample(const KilnNnModel *m, const void *obs, uint32_t *rng_state)
{
    if (!m || !rng_state || m->dtype != KILN_NN_DTYPE_F32)
        return -1;

    float logits[KILN_NN_MAX_ACTIONS];
    if (!kiln_nn_forward(m, obs, logits))
        return -1;

    /* Softmax via stable max-subtract + linear approximation of exp.
     * Good enough for small discrete action spaces on console. */
    float max_l = logits[0];
    for (int i = 1; i < (int)m->action_dim; i++)
        if (logits[i] > max_l) max_l = logits[i];

    float sum = 0.f;
    for (int i = 0; i < (int)m->action_dim; i++) {
        float x = logits[i] - max_l;
        /* 1 + x + x²/2  (first three terms of exp) clamped */
        float e = 1.f + x + 0.5f * x * x;
        if (e < 0.01f) e = 0.01f;
        logits[i] = e;
        sum += e;
    }
    if (sum <= 0.f) return 0;

    float r = (float)(xorshift32(rng_state) & 0xFFFFFFu) / (float)0x1000000u;
    float cum = 0.f;
    for (int i = 0; i < (int)m->action_dim; i++) {
        cum += logits[i] / sum;
        if (r <= cum) return i;
    }
    return (int)m->action_dim - 1;
}

/* ── Observation packing ──────────────────────────────────────────────── */

void kiln_nn_pack_obs_i8(int8_t *dst, size_t n, const float *src)
{
    for (size_t i = 0; i < n; i++) {
        float v = clampf(src[i], -1.f, 1.f);
        dst[i] = (int8_t)(v * 127.f);
    }
}
