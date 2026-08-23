// SPDX-License-Identifier: MIT
//
// kiln_rng.h — a seedable PRNG for any game that needs dice, loot rolls, or
// unpredictable events. xorshift64* (Marsaglia): one 64-bit state, one
// multiply, three shifts per draw. Good statistical quality for a party
// game; not cryptographic, and not claimed to be.
//
// Why this is here and not "just call rand()": newlib's rand() pulls in
// stateful globals and (depending on configuration) locks, and there is no
// contract on its period or seed semantics across versions. A party game
// wants reproducible rolls from a known seed (so a replay or a debug log can
// pin down "why did the CPU player roll a 6 here"), and it wants a per-table
// PRNG instance rather than one global — a game's dice and its board event
// scheduler should not share state.
//
// Single precision on console, but the PRNG draws are 64-bit integer math;
// the float helper exists because the dice and event tables want [0,1)
// variates, not because the engine uses floats for randomness.

#ifndef KILN_RNG_H
#define KILN_RNG_H

#include <stdint.h>

typedef struct {
    uint64_t state;
} KilnRng;

// Seed must be non-zero. A zero seed is forcibly remapped to 1 inside, so a
// caller passing 0 still gets a working sequence rather than a stuck one.
void     kiln_rng_seed(KilnRng *r, uint64_t seed);

// Raw 32-bit draw. xorshift64* returns a full 64-bit value; this folds the
// high half down so callers storing the result in a u32 (e.g. dice tables)
// don't have to do it themselves.
uint32_t kiln_rng_u32(KilnRng *r);

// Uniform float in [0, 1). 24 bits of mantissa used — single precision
// cannot represent more, so drawing more would be wasted work.
float    kiln_rng_f32(KilnRng *r);

// Uniform integer in [lo, hi). lo inclusive, hi exclusive; kiln_rng_range(r,
// 0, 6) returns 0..5 like a die face index.
int      kiln_rng_range(KilnRng *r, int lo, int hi);

#endif // KILN_RNG_H