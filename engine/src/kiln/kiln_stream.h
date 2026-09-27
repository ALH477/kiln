/* SPDX-License-Identifier: MIT
 *
 * kiln_stream.h — priority/budget admission pacer for streamed data.
 *
 * ── Why this exists ────────────────────────────────────────────────────
 * fig_room, fig_tile, fig_asset and fig_cache are four independent
 * systems: nothing connects room/tile residency to actually fetching an
 * asset. fig_room has NO per-frame load budget at all (a whole
 * cross-shaped set can load synchronously in one frame); fig_tile's only
 * budget (load_budget) is a flat count, serviced in slot-scan order, not by
 * distance or urgency. Neither can answer "how much am I about to spend
 * this frame" before spending it.
 *
 * fig_stream is the missing piece: a pacer, NOT a scheduler. There is no
 * async I/O anywhere in this engine or platform — every fig_asset_model/
 * fig_asset_sprite call is still a single blocking call. What this module
 * decides is WHICH of the currently-outstanding requests gets that blocking
 * call issued this frame, in priority order, under a real byte + count
 * budget, deferring the rest to a later frame exactly the way fig_tile's
 * TILE_PENDING already does for its own narrower case.
 *
 * ── Why pure logic, no fig_asset / fig_cache / fig_room / fig_tile
 *    knowledge ───────────────────────────────────────────────────────────
 * Same split as fig_voxel (pure logic, host-testable) / fig_voxmesh
 * (Tiny3D-facing glue): fig_stream operates on opaque (key, urgency, rank,
 * byte_cost, tag) requests only. The console-only binding to real assets and
 * the room/tile callback adapters live in kiln_streamio.h, which is NOT
 * host-buildable (it pulls in kiln_asset.h's <t3d/t3dmodel.h>). Keeping the
 * admission POLICY host-buildable is what lets nix/checks/kiln-logic.nix
 * assert on the priority ordering, the eviction rule, and the budget math
 * natively at -Werror, the same way it already does for fig_cache's handle
 * packing and fig_lod's threshold selection.
 *
 * ── Pool-full eviction, copied from kiln_event.c on purpose ────────────
 * A new request may only evict a PENDING slot, and only if it is strictly
 * higher priority than the least-important PENDING occupant; otherwise the
 * new request itself is dropped. This is fig_event_post's exact rule,
 * reused rather than reinvented — a scarce flat pool under contention is the
 * same problem there and here, and there is no reason for the two policies
 * to disagree. ADMITTED slots are never eviction candidates: they were
 * already chosen this frame and fig_streamio_pump is expected to complete
 * every admitted slot before the next fig_stream_frame_begin runs.
 *
 * ── Priority ordering ──────────────────────────────────────────────────
 * Higher FigStreamUrgency wins; within a tier, smaller `rank` wins (the
 * caller computes rank, typically a squared distance to the camera — this
 * module never touches fm_vec3_t, the same decoupling fig_tile's own LOD
 * selector callback already has from the camera).
 *
 * ── Budget admission is priority-first, not bin-packing ────────────────
 * fig_stream_frame_begin repeatedly admits the single highest-priority
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
#ifndef FIG_STREAM_H
#define FIG_STREAM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FIG_STREAM_MAX_PENDING
/** Pool capacity. 64 covers a 5x5 tile window (25 slots) plus
 *  FIG_ROOM_MAX_LOADED (64) headroom without sizing to both grids' worst
 *  case simultaneously. Override before including the header if a game
 *  streams more than this at once. */
#define FIG_STREAM_MAX_PENDING 64
#endif

/** Key length. Matches FIG_CACHE_MAX_KEY_LEN by convention only —
 *  fig_stream stays cache-agnostic and does not include kiln_cache.h. */
#define FIG_STREAM_MAX_KEY_LEN 64

typedef enum {
    FIG_STREAM_PREFETCH = 0,  /**< speculative; never blocks anything visible */
    FIG_STREAM_NORMAL   = 1,  /**< newly entered the desired set              */
    FIG_STREAM_URGENT   = 2,  /**< camera already inside/adjacent; needed now */
} FigStreamUrgency;

/** A handle that survives slot reuse. Packed like FigCacheHandle: bits
 *  0-15 = index+1 (biased so slot 0 / generation 0 cannot collide with
 *  INVALID == 0 — the exact bug kiln_cache.c's handle packing once had,
 *  fixed there and not reintroduced here), bits 16-23 = generation. */
typedef uint32_t FigStreamHandle;
#define FIG_STREAM_HANDLE_INVALID 0u

typedef struct {
    char     key[FIG_STREAM_MAX_KEY_LEN];
    uint32_t byte_cost;          /**< caller estimate, e.g. fig_asset_size  */
    FigStreamUrgency urgency;
    float    rank;               /**< tie-break within a tier; smaller wins  */
    void    *tag;                /**< opaque; round-tripped on completion    */
    uint8_t  state;              /**< FigStreamSlotState                    */
    uint8_t  generation;
} FigStreamSlot;

typedef enum {
    FIG_STREAM_SLOT_FREE = 0,
    FIG_STREAM_SLOT_PENDING,
    FIG_STREAM_SLOT_ADMITTED,
} FigStreamSlotState;

typedef struct {
    /** Max new admissions per frame. Generalises fig_tile's load_budget
     *  (which only ever throttled tiles) to cover rooms too. */
    uint8_t  max_admits_per_frame;
    /** Max total byte_cost admitted per frame. The genuinely new axis —
     *  neither fig_room nor fig_tile size anything today. */
    uint32_t max_bytes_per_frame;
} FigStreamBudget;

typedef struct {
    FigStreamSlot   slots[FIG_STREAM_MAX_PENDING];
    FigStreamBudget budget;

    uint8_t  admits_this_frame;
    uint32_t bytes_admitted_this_frame;

    uint32_t dropped_total;   /**< pool full, nothing lower-priority to evict */
    uint16_t high_water;      /**< highest simultaneous occupancy ever seen  */
} FigStream;

void fig_stream_init(FigStream *s, FigStreamBudget budget);

/** Request a load. Idempotent: re-requesting an already pending/admitted
 *  key+tag pair just refreshes urgency/rank in place (a tile's urgency
 *  should escalate as the camera approaches without duplicating the slot)
 *  and returns the existing handle.
 *
 *  On a fresh request, may evict the lowest-priority PENDING slot if the
 *  pool is full and this request is strictly higher priority (see file
 *  comment); otherwise increments dropped_total and returns
 *  FIG_STREAM_HANDLE_INVALID. `tag` is never dereferenced — the caller's
 *  problem entirely. */
FigStreamHandle fig_stream_request(FigStream *s, const char *key,
                                     FigStreamUrgency urgency, float rank,
                                     uint32_t byte_cost, void *tag);

/** Cancel a still-outstanding (PENDING or ADMITTED-but-not-yet-completed)
 *  request, e.g. a tile that scrolled back out of the window before its
 *  load ran. Returns 0 if removed, -1 if the handle is stale or already
 *  completed. */
int fig_stream_cancel(FigStream *s, FigStreamHandle h);

/** Once per frame, before issuing any real I/O: resets the per-frame
 *  counters and admits PENDING requests in priority order under budget.
 *  See the file comment for the exact admission rule. */
void fig_stream_frame_begin(FigStream *s);

/** Iterate this frame's ADMITTED slots in slot order. The caller (
 *  fig_streamio_pump) issues the real load for each, then calls
 *  fig_stream_complete. */
FigStreamSlot *fig_stream_first_admitted(FigStream *s);
FigStreamSlot *fig_stream_next_admitted(FigStream *s, FigStreamSlot *cur);

/** Mark a request's underlying work finished (success or failure) and free
 *  its slot. Idempotent with fig_stream_cancel in effect (either one frees
 *  the slot); calling both on the same handle is a caller bug, not
 *  something this module needs to guard against — the second call simply
 *  finds a stale handle. */
void fig_stream_complete(FigStream *s, FigStreamHandle h);

/** Total outstanding requests (PENDING + ADMITTED) — the pool-occupancy
 *  gauge: how close to FIG_STREAM_MAX_PENDING the caller is running. */
uint8_t fig_stream_pending_count(const FigStream *s);

uint32_t fig_stream_bytes_admitted_this_frame(const FigStream *s);
uint8_t  fig_stream_admits_this_frame(const FigStream *s);
uint32_t fig_stream_dropped_total(const FigStream *s);
uint16_t fig_stream_high_water(const FigStream *s);

#ifdef __cplusplus
}
#endif

#endif /* FIG_STREAM_H */
