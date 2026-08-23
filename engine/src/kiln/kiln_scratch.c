/* SPDX-License-Identifier: MIT
 *
 * kiln_scratch.c — per-frame bump allocator implementation.
 */
#include "kiln_scratch.h"

void kiln_scratch_init(KilnScratch *s)
{
    s->offset = 0;
    s->active = 0;
}

void kiln_scratch_begin(KilnScratch *s)
{
    s->active ^= 1;
    s->offset = 0;
}

void *kiln_scratch_alloc(KilnScratch *s, size_t bytes)
{
    uint32_t aligned = (uint32_t)((bytes + 15u) & ~15u);
    if (s->offset + aligned > KILN_SCRATCH_SIZE)
        return NULL;
    void *ptr = &s->memory[s->active][s->offset];
    s->offset += aligned;
    return ptr;
}

void *kiln_scratch_alloc_uncached(KilnScratch *s, size_t bytes)
{
    void *ptr = kiln_scratch_alloc(s, bytes);
    return ptr ? UncachedAddr(ptr) : NULL;
}

T3DMat4FP *kiln_scratch_mat4fp(KilnScratch *s)
{
    return (T3DMat4FP *)kiln_scratch_alloc_uncached(s, sizeof(T3DMat4FP));
}