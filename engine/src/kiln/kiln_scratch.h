/* SPDX-License-Identifier: MIT
 *
 * kiln_scratch.h — per-frame bump allocator for transient render data.
 *
 * ── Why a bump allocator ────────────────────────────────────────────────
 * Every frame the renderer needs temporary memory: fixed-point transform
 * matrices for the RSP, skeleton poses, sorted draw-order arrays, particle
 * buffers. These are all born and dead within one frame. A bump allocator
 * is the cheapest possible allocator — one pointer, one add, one reset —
 * and it produces zero fragmentation, which matters on a 4 MB RDRAM
 * console where the heap is small and a fragmented one is fatal.
 *
 * ── Why double-buffered ────────────────────────────────────────────────
 * The RSP and RDP are asynchronous: a display list built this frame may
 * still be executing when the next frame begins. A single buffer would
 * require a `rspq_wait` at frame start — stalling the CPU on the RDP —
 * or risk overwriting a display list still in flight. Double-buffering
 * alternates between two halves: frame N writes to half A while the RSP
 * may still be reading from half B, then N+1 writes to B while A is in
 * flight. The swap is one XOR. The cost is double the memory; on a 64 KB
 * pool that is 128 KB, under 3% of RDRAM, and the CPU never waits on the
 * RSP to start a frame.
 *
 * ── Alignment ─────────────────────────────────────────────────────────
 * T3DMat4FP is 64 bytes and the RSP reads it via DMA, which requires
 * uncached + 16-byte aligned. The pool itself is 16-byte aligned (the
 * __attribute__ on the struct enforces it), and every allocation rounds
 * up to 16 bytes. T3DMat4FP allocations additionally return the
 * UncachedAddr alias so the CPU writes go straight to RDRAM without
 * polluting the data cache.
 *
 * ── What this is NOT ──────────────────────────────────────────────────
 * Not a general allocator. Not for assets, actors, or anything that
 * outlives a frame. Not thread-safe (the frame is single-threaded on the
 * VR4300). Not a replacement for malloc_uncached — that is for
 * long-lived GPU resources (vertex buffers, textures); this is for
 * ephemeral data that dies at frame end.
 *
 * Inspired by the frame-allocator pattern used in N64 homebrew
 * (lambertjamesd/n64brew2025's frame_alloc) but designed from first
 * principles: double-buffered instead of single, configurable alignment,
 * typed helpers, and an explicit uncached-memory path.
 */
#ifndef KILN_SCRATCH_H
#define KILN_SCRATCH_H

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KILN_SCRATCH_SIZE
/** Per-half size in bytes. Total pool is 2× this. 64 KB is enough for
 *  ~256 T3DMat4FP transforms plus particle buffers and sort arrays. */
#define KILN_SCRATCH_SIZE 65536
#endif

/** Double-buffered bump allocator for per-frame transient memory.
 *  Embed by value (it is 2×KILN_SCRATCH_SIZE + a few words). */
typedef struct {
    uint8_t memory[2][KILN_SCRATCH_SIZE] __attribute__((aligned(16)));
    uint32_t offset;      /**< current write position within active half  */
    uint8_t  active;      /**< 0 or 1 — which half the CPU writes to     */
} KilnScratch;

/** Initialise (zero the offset, start in half 0). */
void kiln_scratch_init(KilnScratch *s);

/** Call at the beginning of each frame: swap to the other half and reset
 *  the write offset. The previous half may still be read by the RSP/RDP. */
void kiln_scratch_begin(KilnScratch *s);

/** Allocate `bytes` from the active half, 16-byte aligned. Returns NULL
 *  if the half is full (the caller should size the pool to never hit this
 *  in practice; a NULL return means a debugf-worthy overflow). */
void *kiln_scratch_alloc(KilnScratch *s, size_t bytes);

/** Allocate and return the UncachedAddr alias. Use for T3DMat4FP and
 *  any data the RSP will DMA — writes bypass the data cache. */
void *kiln_scratch_alloc_uncached(KilnScratch *s, size_t bytes);

/** Allocate one T3DMat4FP, uncached. The most common per-frame alloc. */
T3DMat4FP *kiln_scratch_mat4fp(KilnScratch *s);

/** How many bytes are left in the active half. */
static inline size_t kiln_scratch_remaining(const KilnScratch *s) {
    return KILN_SCRATCH_SIZE - s->offset;
}

#ifdef __cplusplus
}
#endif

#endif /* KILN_SCRATCH_H */