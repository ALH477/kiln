// SPDX-License-Identifier: MIT

#include "kiln_radio.h"
#include "kiln_rng.h"

uint64_t kiln_radio_mix_seed(uint32_t ticks, uint32_t extra)
{
    uint64_t s = ((uint64_t)ticks << 32) ^ (uint64_t)extra;
    s ^= 0xA5A5A5A5A5A5A5A5ULL;
    return s ? s : 0x9E3779B97F4A7C15ULL;
}

int kiln_radio_pick(uint64_t seed, int n)
{
    if (n <= 1) return 0;
    KilnRng r;
    kiln_rng_seed(&r, seed);
    return kiln_rng_range(&r, 0, n);
}
