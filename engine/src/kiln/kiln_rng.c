// SPDX-License-Identifier: MIT

#include "kiln_rng.h"

void fig_rng_seed(FigRng *r, uint64_t seed)
{
    r->state = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

uint32_t fig_rng_u32(FigRng *r)
{
    uint64_t x = r->state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->state = x;
    // xorshift64*'s multiplier, high half only — that's where the entropy
    // concentrates. Folded to u32 for callers that want a compact draw.
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

float fig_rng_f32(FigRng *r)
{
    // 24-bit mantissa, masked off the top of a u32 draw. Single precision
    // cannot represent more precision than this, so don't draw more.
    uint32_t u = fig_rng_u32(r) >> 8;
    return (float)u * (1.0f / 16777216.0f);
}

int fig_rng_range(FigRng *r, int lo, int hi)
{
    if (hi <= lo) return lo;
    int span = hi - lo;
    // u32 % span is fine for small spans (dice, count tables); the bias for
    // a span that doesn't divide 2^32 is sub-1-per-billion for any realistic
    // game-side span, well under the noise floor of "is this fair?".
    return lo + (int)(fig_rng_u32(r) % (uint32_t)span);
}
