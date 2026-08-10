// SPDX-License-Identifier: MPL-2.0

#include "m64_rng.h"

void m64_rng_seed(M64Rng *r, uint64_t seed)
{
    r->state = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

uint32_t m64_rng_u32(M64Rng *r)
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

float m64_rng_f32(M64Rng *r)
{
    // 24-bit mantissa, masked off the top of a u32 draw. Single precision
    // cannot represent more precision than this, so don't draw more.
    uint32_t u = m64_rng_u32(r) >> 8;
    return (float)u * (1.0f / 16777216.0f);
}

int m64_rng_range(M64Rng *r, int lo, int hi)
{
    if (hi <= lo) return lo;
    int span = hi - lo;
    // u32 % span is fine for small spans (dice, count tables); the bias for
    // a span that doesn't divide 2^32 is sub-1-per-billion for any realistic
    // game-side span, well under the noise floor of "is this fair?".
    return lo + (int)(m64_rng_u32(r) % (uint32_t)span);
}