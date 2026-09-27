// SPDX-License-Identifier: MIT
//
// kiln_dice.h — a small dice table helper for party games.
//
// A "die" here is a face table plus an optional bias per face, not a fixed
// 1..6 roll. Two reasons:
//
//   1. A party game's themed die commonly wants "occasional special faces" —
//      a small handful of faces that, when rolled, trigger unusual effects
//      beyond a plain move count. That is naturally a face table with extra
//      face ids, not a uniform 1..6.
//
//   2. A character passive like "better dice odds when behind" is a bias
//      tweak on a per-roll basis, not a different die. Letting the caller
//      pass a bias array per roll means the passive is one line in the
//      game-side code: bump the bias on the high-value faces when behind.
//
// Up to 16 faces — enough for a "1-6 with occasional special faces" die plus
// headroom, and small enough that the bias table fits in 16 bytes. The roll
// is O(faces) via cumulative-bucket sampling, which is
// cheaper than it sounds at face_count <= 16.

#ifndef FIG_DICE_H
#define FIG_DICE_H

#include <stdint.h>
#include "kiln_rng.h"

#define FIG_DICE_MAX_FACES 16

typedef struct {
    uint8_t face_count;
    // Face ids are caller-defined. 1..6 for a standard die, or arbitrary
    // tags for special faces (e.g. 0x10 = "Crystal Bud face", 0x11 = "Lose
    // a turn"). The dice helper treats them as opaque.
    uint8_t faces[FIG_DICE_MAX_FACES];
    // Per-face weight, in 1/256 units. A uniform die has every bias = 1.
    // Zero is allowed and means "this face can never be rolled" — useful
    // for toggling special faces off in modes that shouldn't see them.
    uint8_t bias[FIG_DICE_MAX_FACES];
} FigDice;

// Initialise with a face table and an optional bias table. Pass NULL for
// `bias` to get a uniform die. face_count must be > 0 and <=
// FIG_DICE_MAX_FACES.
void fig_dice_init(FigDice *d, const uint8_t *faces, const uint8_t *bias,
                   uint8_t face_count);

// Convenience for a standard 1..N uniform die.
void fig_dice_init_uniform(FigDice *d, uint8_t face_count);

// Roll once against the given PRNG instance. Returns one of the stored face
// ids. If every bias is zero (degenerate), returns the first face.
int  fig_dice_roll(const FigDice *d, FigRng *rng);

// Tweak a face's bias. No-op if the face index is out of range. This is the
// hook Glimmer's passive uses to bump high-value faces when behind.
void fig_dice_set_bias(FigDice *d, uint8_t face_index, uint8_t weight);

#endif // FIG_DICE_H