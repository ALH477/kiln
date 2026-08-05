/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_scratch.c — per-frame bump allocator implementation.
 */
#include "m64_scratch.h"

void m64_scratch_init(M64Scratch *s)
{
    s->offset = 0;
    s->active = 0;
}

void m64_scratch_begin(M64Scratch *s)
{
    s->active ^= 1;
    s->offset = 0;
}

void *m64_scratch_alloc(M64Scratch *s, size_t bytes)
{
    uint32_t aligned = (uint32_t)((bytes + 15u) & ~15u);
    if (s->offset + aligned > M64_SCRATCH_SIZE)
        return NULL;
    void *ptr = &s->memory[s->active][s->offset];
    s->offset += aligned;
    return ptr;
}

void *m64_scratch_alloc_uncached(M64Scratch *s, size_t bytes)
{
    void *ptr = m64_scratch_alloc(s, bytes);
    return ptr ? UncachedAddr(ptr) : NULL;
}

T3DMat4FP *m64_scratch_mat4fp(M64Scratch *s)
{
    return (T3DMat4FP *)m64_scratch_alloc_uncached(s, sizeof(T3DMat4FP));
}