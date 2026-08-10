// SPDX-License-Identifier: MPL-2.0
//
// m64_rng.h — a seedable PRNG for any game that needs dice, loot rolls, or
// unpredictable events. xorshift64* (Marsaglia): one 64-bit state, one
// multiply, three shifts per draw. Good statistical quality for a party
// game; not cryptographic, and not claimed to be.
//
// Why this is here and not "just call rand()": newlib's rand() pulls in
// stateful globals and (depending on configuration) locks, and there is no
// contract on its period or seed semantics across versions. A party game
// wants reproducible rolls from a known seed (so a replay or a debug log can
// pin down "why did the CPU goblin roll a 6 here"), and it wants a per-table
// PRNG instance rather than one global — Ganja Goblin's dice and its board
// event scheduler should not share state.
//
// Single precision on console, but the PRNG draws are 64-bit integer math;
// the float helper exists because the dice and event tables want [0,1)
// variates, not because the engine uses floats for randomness.

#ifndef M64_RNG_H
#define M64_RNG_H

#include <stdint.h>

typedef struct {
    uint64_t state;
} M64Rng;

// Seed must be non-zero. A zero seed is forcibly remapped to 1 inside, so a
// caller passing 0 still gets a working sequence rather than a stuck one.
void     m64_rng_seed(M64Rng *r, uint64_t seed);

// Raw 32-bit draw. xorshift64* returns a full 64-bit value; this folds the
// high half down so callers storing the result in a u32 (e.g. dice tables)
// don't have to do it themselves.
uint32_t m64_rng_u32(M64Rng *r);

// Uniform float in [0, 1). 24 bits of mantissa used — single precision
// cannot represent more, so drawing more would be wasted work.
float    m64_rng_f32(M64Rng *r);

// Uniform integer in [lo, hi). lo inclusive, hi exclusive; m64_rng_range(r,
// 0, 6) returns 0..5 like a die face index.
int      m64_rng_range(M64Rng *r, int lo, int hi);

#endif // M64_RNG_H