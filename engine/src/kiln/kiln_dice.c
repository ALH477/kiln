// SPDX-License-Identifier: MIT

#include "kiln_dice.h"

void fig_dice_init(FigDice *d, const uint8_t *faces, const uint8_t *bias,
                   uint8_t face_count)
{
    if (face_count == 0) face_count = 1;
    if (face_count > FIG_DICE_MAX_FACES) face_count = FIG_DICE_MAX_FACES;
    d->face_count = face_count;
    for (uint8_t i = 0; i < face_count; i++) {
        d->faces[i] = faces[i];
        d->bias[i] = bias ? bias[i] : 1;
    }
}

void fig_dice_init_uniform(FigDice *d, uint8_t face_count)
{
    if (face_count == 0) face_count = 1;
    if (face_count > FIG_DICE_MAX_FACES) face_count = FIG_DICE_MAX_FACES;
    d->face_count = face_count;
    for (uint8_t i = 0; i < face_count; i++) {
        d->faces[i] = (uint8_t)(i + 1);
        d->bias[i] = 1;
    }
}

int fig_dice_roll(const FigDice *d, FigRng *rng)
{
    // Cumulative-bucket sampling: pick a target in [0, total), walk the
    // cumulative sum, return the face whose bucket contains the target.
    // O(faces) but face_count <= 16, so cheaper than building a prefix
    // table for a one-shot roll.
    uint32_t total = 0;
    for (uint8_t i = 0; i < d->face_count; i++) total += d->bias[i];
    if (total == 0) return d->faces[0];

    uint32_t target = fig_rng_u32(rng) % total;
    uint32_t acc = 0;
    for (uint8_t i = 0; i < d->face_count; i++) {
        acc += d->bias[i];
        if (target < acc) return d->faces[i];
    }
    return d->faces[d->face_count - 1];
}

void fig_dice_set_bias(FigDice *d, uint8_t face_index, uint8_t weight)
{
    if (face_index < d->face_count) d->bias[face_index] = weight;
}