/* SPDX-License-Identifier: MIT
 *
 * kiln_stream.h — priority/budget admission pacer for streamed data.
 *
 * ── Why this exists ────────────────────────────────────────────────────
 * kiln_room, kiln_tile, kiln_asset and kiln_cache are four independent
 * systems: nothing connects room/tile residency to actually fetching an
 * asset. kiln_room has NO per-frame load budget at all (a whole
 * cross-shaped set can load synchronously in one frame); kiln_tile's only
 * budget (load_budget) is a flat count, serviced in slot-scan order, not by
 * distance or urgency. Neither can answer "how much am I about to spend
 * this frame" before spending it.
 *
 * kiln_stream is the missing piece: a pacer, NOT a scheduler. There is no
 * async I/O anywhere in this engine or platform — every kiln_asset_model/
 * kiln_asset_sprite call is still a single blocking call. What this module
 * decides is WHICH of the currently-outstanding requests gets that blocking
 * call issued this frame, in priority order, under a real byte + count
 * budget, deferring the rest to a later frame exactly the way kiln_tile's
 * TILE_PENDING already does for its own narrower case.
 *
 * ── Why pure logic, no kiln_asset / kiln_cache / kiln_room / kiln_tile
 *    knowledge ───────────────────────────────────────────────────────────
 * Same split as kiln_voxel (pure logic, host-testable) / kiln_voxmesh
 * (Tiny3D-facing glue): kiln_stream operates on opaque (key, urgency, rank,
 * byte_cost, tag) requests only. The console-only binding to real assets and
 * the room/tile callback adapters live in kiln_streamio.h, which is NOT
 * host-buildable (it pulls in kiln_asset.h's <t3d/t3dmodel.h>). Keeping the
 * admission POLICY host-buildable is what lets nix/checks/kiln-logic.nix
 * assert on the priority ordering, the eviction rule, and the budget math
 * natively at -Werror, the same way it already does for kiln_cache's handle
 * packing and kiln_lod's threshold selection.
 *
 * ── Pool-full eviction, copied from kiln_event.c on purpose ────────────
 * A new request may only evict a PENDING slot, and only if it is strictly
 * higher priority than the least-important PENDING occupant; otherwise the
 * new request itself is dropped. This is kiln_event_post's exact rule,
 * reused rather than reinvented — a scarce flat pool under contention is the
 * same problem there and here, and there is no reason for the two policies
 * to disagree. ADMITTED slots are never eviction candidates: they were
 * already chosen this frame and kiln_streamio_pump is expected to complete
 * every admitted slot before the next kiln_stream_frame_begin runs.
 *
 * ── Priority ordering ──────────────────────────────────────────────────
 * Higher KilnStreamUrgency wins; within a tier, smaller `rank` wins (the
 * caller computes rank, typically a squared distance to the camera — this
 * module never touches fm_vec3_t, the same decoupling kiln_tile's own LOD
 * selector callback already has from the camera).
 *
 * ── Budget admission is priority-first, not bin-packing ────────────────
 * kiln_stream_frame_begin repeatedly admits the single highest-priority
 * PENDING request that still fits the remaining byte budget. It stops the
 * instant the highest-priority remaining candidate does not fit, rather
 * than skipping ahead to a smaller, lower-priority one that would — a
 * bin-packing scan would let an unimportant small asset jump ahead of an
 * important large one, which is exactly backwards for a priority pacer.
 * The one exception: if NOTHING has been admitted yet this frame and the
 * single highest-priority candidate still does not fit the whole budget on
 * its own, it is admitted anyway. Without that rule a request whose
 * byte_cost exceeds max_bytes_per_frame would never be serviced at all.
 */
#ifndef KILN_STREAM_H
#define KILN_STREAM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KILN_STREAM_MAX_PENDING
/** Pool capacity. 64 covers a 5x5 tile window (25 slots) plus
 *  KILN_ROOM_MAX_LOADED (64) headroom without sizing to both grids' worst
 *  case simultaneously. Override before including the header if a game
 *  streams more than this at once. */
#define KILN_STREAM_MAX_PENDING 64
#endif

/** Key length. Matches KILN_CACHE_MAX_KEY_LEN by convention only —
 *  kiln_stream stays cache-agnostic and does not include kiln_cache.h. */
#define KILN_STREAM_MAX_KEY_LEN 64

typedef enum {
    KILN_STREAM_PREFETCH = 0,  /**< speculative; never blocks anything visible */
    KILN_STREAM_NORMAL   = 1,  /**< newly entered the desired set              */
    KILN_STREAM_URGENT   = 2,  /**< camera already inside/adjacent; needed now */
} KilnStreamUrgency;

/** A handle that survives slot reuse. Packed like KilnCacheHandle: bits
 *  0-15 = index+1 (biased so slot 0 / generation 0 cannot collide with
 *  INVALID == 0 — the exact bug kiln_cache.c's handle packing once had,
 *  fixed there and not reintroduced here), bits 16-23 = generation. */
typedef uint32_t KilnStreamHandle;
#define KILN_STREAM_HANDLE_INVALID 0u

typedef struct {
    char     key[KILN_STREAM_MAX_KEY_LEN];
    uint32_t byte_cost;          /**< caller estimate, e.g. kiln_asset_size  */
    KilnStreamUrgency urgency;
    float    rank;               /**< tie-break within a tier; smaller wins  */
    void    *tag;                /**< opaque; round-tripped on completion    */
    uint8_t  state;              /**< KilnStreamSlotState                    */
    uint8_t  generation;
} KilnStreamSlot;

typedef enum {
    KILN_STREAM_SLOT_FREE = 0,
    KILN_STREAM_SLOT_PENDING,
    KILN_STREAM_SLOT_ADMITTED,
} KilnStreamSlotState;

typedef struct {
    /** Max new admissions per frame. Generalises kiln_tile's load_budget
     *  (which only ever throttled tiles) to cover rooms too. */
    uint8_t  max_admits_per_frame;
    /** Max total byte_cost admitted per frame. The genuinely new axis —
     *  neither kiln_room nor kiln_tile size anything today. */
    uint32_t max_bytes_per_frame;
} KilnStreamBudget;

typedef struct {
    KilnStreamSlot   slots[KILN_STREAM_MAX_PENDING];
    KilnStreamBudget budget;

    uint8_t  admits_this_frame;
    uint32_t bytes_admitted_this_frame;

    uint32_t dropped_total;   /**< pool full, nothing lower-priority to evict */
    uint16_t high_water;      /**< highest simultaneous occupancy ever seen  */
} KilnStream;

void kiln_stream_init(KilnStream *s, KilnStreamBudget budget);

/** Request a load. Idempotent: re-requesting an already pending/admitted
 *  key+tag pair just refreshes urgency/rank in place (a tile's urgency
 *  should escalate as the camera approaches without duplicating the slot)
 *  and returns the existing handle.
 *
 *  On a fresh request, may evict the lowest-priority PENDING slot if the
 *  pool is full and this request is strictly higher priority (see file
 *  comment); otherwise increments dropped_total and returns
 *  KILN_STREAM_HANDLE_INVALID. `tag` is never dereferenced — the caller's
 *  problem entirely. */
KilnStreamHandle kiln_stream_request(KilnStream *s, const char *key,
                                     KilnStreamUrgency urgency, float rank,
                                     uint32_t byte_cost, void *tag);

/** Cancel a still-outstanding (PENDING or ADMITTED-but-not-yet-completed)
 *  request, e.g. a tile that scrolled back out of the window before its
 *  load ran. Returns 0 if removed, -1 if the handle is stale or already
 *  completed. */
int kiln_stream_cancel(KilnStream *s, KilnStreamHandle h);

/** Once per frame, before issuing any real I/O: resets the per-frame
 *  counters and admits PENDING requests in priority order under budget.
 *  See the file comment for the exact admission rule. */
void kiln_stream_frame_begin(KilnStream *s);

/** Iterate this frame's ADMITTED slots in slot order. The caller (
 *  kiln_streamio_pump) issues the real load for each, then calls
 *  kiln_stream_complete. */
KilnStreamSlot *kiln_stream_first_admitted(KilnStream *s);
KilnStreamSlot *kiln_stream_next_admitted(KilnStream *s, KilnStreamSlot *cur);

/** Mark a request's underlying work finished (success or failure) and free
 *  its slot. Idempotent with kiln_stream_cancel in effect (either one frees
 *  the slot); calling both on the same handle is a caller bug, not
 *  something this module needs to guard against — the second call simply
 *  finds a stale handle. */
void kiln_stream_complete(KilnStream *s, KilnStreamHandle h);

/** Total outstanding requests (PENDING + ADMITTED) — the pool-occupancy
 *  gauge: how close to KILN_STREAM_MAX_PENDING the caller is running. */
uint8_t kiln_stream_pending_count(const KilnStream *s);

uint32_t kiln_stream_bytes_admitted_this_frame(const KilnStream *s);
uint8_t  kiln_stream_admits_this_frame(const KilnStream *s);
uint32_t kiln_stream_dropped_total(const KilnStream *s);
uint16_t kiln_stream_high_water(const KilnStream *s);

#ifdef __cplusplus
}
#endif

#endif /* KILN_STREAM_H */
