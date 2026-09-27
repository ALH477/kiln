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

#ifndef FIG_RNG_H
#define FIG_RNG_H


/* The prefix migration train (docs/NAMING.md section 9 step 2). Pulled in by
 * every public header (a quoted include, so it resolves both in this tree and
 * in the installed include/kiln prefix) rather than force-included by
 * kiln-inst.mk, because a
 * force-include only reaches builds that include that file — a Nix check or a
 * host build compiling a downstream's sources directly never saw it, and
 * PetaByte-Madness' pm-cine check is what proved that. Deleting the train is
 * still a scripted one-line removal from these headers plus the file itself.
 */
#include "kiln_compat.h"

#include <stdint.h>

typedef struct {
    uint64_t state;
} FigRng;

// Seed must be non-zero. A zero seed is forcibly remapped to 1 inside, so a
// caller passing 0 still gets a working sequence rather than a stuck one.
void     fig_rng_seed(FigRng *r, uint64_t seed);

// Raw 32-bit draw. xorshift64* returns a full 64-bit value; this folds the
// high half down so callers storing the result in a u32 (e.g. dice tables)
// don't have to do it themselves.
uint32_t fig_rng_u32(FigRng *r);

// Uniform float in [0, 1). 24 bits of mantissa used — single precision
// cannot represent more, so drawing more would be wasted work.
float    fig_rng_f32(FigRng *r);

// Uniform integer in [lo, hi). lo inclusive, hi exclusive; fig_rng_range(r,
// 0, 6) returns 0..5 like a die face index.
int      fig_rng_range(FigRng *r, int lo, int hi);

#endif // FIG_RNG_H