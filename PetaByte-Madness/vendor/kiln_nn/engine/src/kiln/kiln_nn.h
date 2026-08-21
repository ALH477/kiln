/* SPDX-License-Identifier: MPL-2.0 */
/**
 * kiln_nn — lightweight quantized neural-network inference for N64 NPC policies.
 *
 * Design goals (Kiln constraints):
 *   - Static allocation only. Caller owns the model buffer and scratch.
 *   - Default path is int8 weights + int32 accumulators (deterministic,
 *     bit-exact between host reference and console).
 *   - Float32 path exists for rapid prototyping.
 *   - No malloc, no libm on the integer path, no heap.
 *   - Policies are small (tens of KB). Training is offline on x86;
 *     this module is inference only.
 *
 * Typical use for an NPC actor:
 *   1. At spawn (or room load) load a policy (const arrays, DFS, or
 *      StreamDB via kiln_asset).
 *   2. Each tick build a compact observation vector from actor state +
 *      world queries.
 *   3. kiln_nn_forward() / kiln_nn_argmax() → discrete action.
 *   4. Map the action to velocity, an event, or dialogue conditioning.
 *
 * The observation / action spaces are the game's responsibility.
 */

#ifndef KILN_NN_H
#define KILN_NN_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration limits (override before include if needed) ─────────── */

#ifndef KILN_NN_MAX_LAYERS
#define KILN_NN_MAX_LAYERS       8
#endif

#ifndef KILN_NN_MAX_WIDTH
#define KILN_NN_MAX_WIDTH        256
#endif

#ifndef KILN_NN_MAX_OBS
#define KILN_NN_MAX_OBS          64
#endif

#ifndef KILN_NN_MAX_ACTIONS
#define KILN_NN_MAX_ACTIONS      32
#endif

/* ── Enums ────────────────────────────────────────────────────────────── */

typedef enum {
    KILN_NN_ACT_LINEAR = 0,
    KILN_NN_ACT_RELU,
    KILN_NN_ACT_HARDTANH,   /* clamp to [-1,1] / int8 range */
    KILN_NN_ACT_TANH_LUT,   /* integer LUT (future) or Padé approx */
} KilnNnActivation;

typedef enum {
    KILN_NN_DTYPE_INT8 = 0, /* preferred production path */
    KILN_NN_DTYPE_F32,      /* prototyping */
} KilnNnDtype;

/* ── Layer descriptor (read-only after load) ──────────────────────────── */

typedef struct {
    uint16_t          in_features;
    uint16_t          out_features;
    KilnNnActivation   act;

    /* int8 path */
    const int8_t     *weights_i8;   /* [out * in], row-major */
    const int32_t    *bias_i32;     /* [out] or NULL */
    int32_t           scale_shift;  /* power-of-two right shift after matvec */

    /* float path (only when dtype == F32) */
    const float      *weights_f32;
    const float      *bias_f32;
} KilnNnLayer;

/* ── Model ────────────────────────────────────────────────────────────── */

typedef struct {
    KilnNnDtype        dtype;
    uint8_t           n_layers;
    uint16_t          obs_dim;
    uint16_t          action_dim;
    KilnNnLayer        layers[KILN_NN_MAX_LAYERS];
    const int16_t    *tanh_lut;     /* optional 256-entry LUT, or NULL */
    void             *scratch;
    size_t            scratch_bytes;
} KilnNnModel;

/* ── API ──────────────────────────────────────────────────────────────── */

/**
 * Initialise a model from a descriptor table (usually generated C arrays
 * emitted by tools/nn/train_export.py). Does not allocate.
 *
 * Returns false if the descriptor is malformed or exceeds compile-time limits.
 */
bool kiln_nn_init(KilnNnModel *m,
                 KilnNnDtype dtype,
                 uint8_t n_layers,
                 const KilnNnLayer *layers,
                 uint16_t obs_dim,
                 uint16_t action_dim,
                 const int16_t *tanh_lut,
                 void *scratch,
                 size_t scratch_bytes);

/**
 * Minimum scratch size required for a model of the given shape.
 * Call once at boot / load time to size the arena.
 */
size_t kiln_nn_scratch_bytes(uint16_t max_width, uint8_t n_layers);

/**
 * Forward pass.
 *
 * obs    — int8 or float32 observation of length model->obs_dim
 * logits — output buffer of length model->action_dim
 *          (int32 for int8 path, float for f32 path)
 *
 * Returns false on dimension mismatch or missing scratch.
 */
bool kiln_nn_forward(const KilnNnModel *m, const void *obs, void *logits);

/**
 * Run forward and return the argmax action index.
 * Preferred path for discrete NPC policies.
 */
int kiln_nn_argmax(const KilnNnModel *m, const void *obs);

/**
 * Softmax + multinomial sample (float path only).
 * Uses a libm-free approximation. Prefer argmax in production.
 * Returns -1 on error.
 */
int kiln_nn_sample(const KilnNnModel *m, const void *obs, uint32_t *rng_state);

/**
 * Pack float observations in roughly [-1,1] into int8 for the int8 path.
 */
void kiln_nn_pack_obs_i8(int8_t *dst, size_t n, const float *src);

#ifdef __cplusplus
}
#endif

#endif /* KILN_NN_H */
