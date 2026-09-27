/* SPDX-License-Identifier: MIT
 *
 * kiln_scratch.c — per-frame bump allocator implementation.
 */
#include "kiln_scratch.h"

void fig_scratch_init(FigScratch *s)
{
    s->offset = 0;
    s->active = 0;
}

void fig_scratch_begin(FigScratch *s)
{
    s->active ^= 1;
    s->offset = 0;
}

void *fig_scratch_alloc(FigScratch *s, size_t bytes)
{
    uint32_t aligned = (uint32_t)((bytes + 15u) & ~15u);
    if (s->offset + aligned > FIG_SCRATCH_SIZE)
        return NULL;
    void *ptr = &s->memory[s->active][s->offset];
    s->offset += aligned;
    return ptr;
}

void *fig_scratch_alloc_uncached(FigScratch *s, size_t bytes)
{
    void *ptr = fig_scratch_alloc(s, bytes);
    return ptr ? UncachedAddr(ptr) : NULL;
}

T3DMat4FP *fig_scratch_mat4fp(FigScratch *s)
{
    return (T3DMat4FP *)fig_scratch_alloc_uncached(s, sizeof(T3DMat4FP));
}