/* SPDX-License-Identifier: MIT
 *
 * kiln_streamio.h — binds fig_stream's admission policy to real
 * fig_asset + fig_cache loads, and adapts it to fig_room / fig_tile's
 * existing callback contracts.
 *
 * Compiles natively (fig_asset, fig_room, fig_tile and fig_cache are all
 * host-buildable now that plat/host/include ships a t3dmodel.h shim), so
 * both this and fig_stream sit in HOST_MODULES and get the -Werror second
 * opinion from nix/checks/kiln-parity.nix. Only fig_stream's admission
 * POLICY is asserted on at runtime by nix/checks/kiln-logic.nix, though —
 * this file's fig_asset_model/fig_asset_sprite/t3d_model_free/sprite_free
 * calls need real StreamDB content and a linked host_t3dmodel.c to exercise
 * end to end, which is a heavier host check left as a follow-on (see the
 * plan's "optional follow-on" note) rather than required for this to land.
 *
 * ── Zero new callback contract ──────────────────────────────────────────
 * fig_streamio_room_on_load/_on_unload and fig_streamio_tile_on_load/
 * _on_unload are literal FigRoomLoadFn/UnloadFn and FigTileLoadFn/UnloadFn
 * implementations — a game that wants paced streaming points its existing
 * fig_room_system_init / fig_tile_init callback slots at these instead of
 * writing its own; a game that doesn't need pacing keeps its own loaders
 * exactly as before. No edits to kiln_room.h or kiln_tile.h were needed or
 * made.
 *
 * ── The fig_tile load_budget = 255 requirement ─────────────────────────
 * fig_tile's own load_fn is called SYNCHRONOUSLY and is expected to return
 * the loaded user_data pointer immediately — but the whole point here is
 * that the real, expensive fig_asset+fig_cache call must NOT happen
 * synchronously inside that callback, or fig_stream's budget/priority
 * ordering would have nothing left to pace. So fig_streamio_tile_on_load
 * does no I/O: it only calls fig_stream_request (an O(1) array insert) and
 * returns NULL. For that to run for every newly-desired tile in the same
 * frame — rather than fig_tile deferring some of them to TILE_PENDING on
 * its OWN flat per-frame counter — the manager must be initialised with
 * `tiles.load_budget = 255`, kiln_tile.h's own documented "synchronous,
 * load-all-immediately" escape hatch. With that set, fig_tile's throttling
 * never engages and fig_stream becomes the sole budget authority; the real
 * load happens later, in fig_streamio_pump, only for admitted requests.
 * fig_streamio_room_on_load needs no such setting — fig_room has no
 * budget of its own to disable.
 *
 * ── How a completed load reaches room->user_mesh / tile->user_data ──────
 * Neither field is written from inside on_load (which returns before the
 * real load has even been requested for admission). fig_streamio_pump
 * writes them once the admitted request's fig_cache_acquire call actually
 * completes, later the same frame, before the draw pass runs. This is safe
 * because both fields are already documented as "the engine never
 * dereferences this; the user's own draw callback does, after checking for
 * NULL" — exactly the state a tile/room whose load is still outstanding is
 * already expected to tolerate.
 */
#ifndef FIG_STREAMIO_H
#define FIG_STREAMIO_H

#include <stdint.h>

#include "kiln_stream.h"
#include "kiln_asset.h"
#include "kiln_cache.h"
#include "kiln_room.h"
#include "kiln_tile.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Binds one FigStream to one asset DB + cache. Own nothing: `db` and
 *  `cache` are caller-opened/initialised and must outlive this. */
typedef struct {
    FigAsset *db;
    FigCache *cache;
    FigStream stream;
    uint32_t   fail_total;  /**< an ADMITTED request's fig_asset_model/
                                 sprite call returned NULL — a content bug
                                 (bad key, missing DB entry), not a budget
                                 one. Distinct RED gauge from
                                 fig_stream_dropped_total, which is a
                                 capacity/tuning problem instead. */
} FigStreamIO;

void fig_streamio_init(FigStreamIO *io, FigAsset *db, FigCache *cache,
                        FigStreamBudget budget);

/** Call once per frame: after fig_stream_frame_begin (implicitly run
 *  inside here) and after this frame's room/tile on_load calls have
 *  submitted their requests, before the draw pass. Issues the real
 *  fig_asset + fig_cache call for every request fig_stream admitted this
 *  frame, writes the result into the owning room's user_mesh or tile's
 *  user_data, and completes the request either way (a genuine asset
 *  failure must not retry forever — see fig_stream_complete). */
void fig_streamio_pump(FigStreamIO *io);

uint32_t fig_streamio_fail_total(const FigStreamIO *io);

/** Which fig_asset accessor a given key resolves through. */
typedef enum {
    FIG_STREAMIO_MODEL = 0,   /**< fig_asset_model / t3d_model_free */
    FIG_STREAMIO_SPRITE = 1,  /**< fig_asset_sprite / sprite_free   */
} FigStreamIOAssetType;

/** Per-room/per-tile bookkeeping: which cache handle (if any) is resident,
 *  which fig_stream request (if any) is still outstanding, and enough to
 *  find the owner again when fig_streamio_pump resolves it later. Exactly
 *  one of `handle`/`req` is ever live at a time (never both INVALID after
 *  on_load, and both INVALID once unloaded). Caller-allocated: one array
 *  entry per FigRoom (indexed by room->id) or per tile grid slot (same
 *  slots_x*slots_y sizing and modulo-wrap indexing as FigTileGrid itself,
 *  per kiln_tile.h's "Slot reuse via modulo wrapping" section). */
typedef struct {
    FigCacheHandle  handle;
    FigStreamHandle req;
    FigStreamIOAssetType asset_type;
    enum { FIG_STREAMIO_OWNER_ROOM, FIG_STREAMIO_OWNER_TILE } owner_kind;
    union {
        struct { FigRoom *room; } room;
        struct { FigTileGrid *grid; int16_t tx, ty; } tile;
    } owner;
} FigStreamIOSlot;

/* ── Room adapter ───────────────────────────────────────────────────── */

/** Builds the StreamDB key for a room's mesh asset. Game-specific — e.g.
 *  snprintf(out, out_len, "rooms/%02u.t3dm", room->id). */
typedef void (*FigStreamIORoomKeyFn)(const FigRoom *room, char *out, size_t out_len);

typedef struct {
    FigStreamIO *io;
    FigStreamIOSlot *slots;         /**< caller-allocated, room_count entries */
    FigStreamIOAssetType asset_type;
    FigStreamIORoomKeyFn key_fn;
    fm_vec3_t camera_pos;            /**< set by the caller once per frame,
                                          before fig_room_system_update */
} FigStreamIORoomBinding;

/** Zeroes `slots` (room_count entries) and binds the callbacks below to
 *  route through `io`. Call once at boot, before fig_room_system_init. */
void fig_streamio_room_bind(FigStreamIORoomBinding *b, FigStreamIO *io,
                             FigStreamIOSlot *slots, uint16_t room_count,
                             FigStreamIOAssetType type,
                             FigStreamIORoomKeyFn key_fn);

/** Pass as FigRoomLoadFn/UnloadFn to fig_room_system_init with
 *  user_ctx = &binding. */
void fig_streamio_room_on_load  (FigRoom *room, void *user);
void fig_streamio_room_on_unload(FigRoom *room, void *user);

/* ── Tile adapter ───────────────────────────────────────────────────── */

/** Builds the StreamDB key for a tile at (tx, ty, lod). Game-specific —
 *  e.g. snprintf(out, out_len, "tiles/%d_%d/lod%u.t3dm", tx, ty, lod). */
typedef void (*FigStreamIOTileKeyFn)(int16_t tx, int16_t ty, uint8_t lod,
                                      char *out, size_t out_len);

typedef struct {
    FigStreamIO *io;
    FigTileGrid *grid;               /**< the grid this binding services */
    FigStreamIOSlot *slots;          /**< caller-allocated,
                                            slots_x * slots_y entries      */
    FigStreamIOAssetType asset_type;
    FigStreamIOTileKeyFn key_fn;
    fm_vec3_t camera_pos;             /**< set by the caller once per frame,
                                            before fig_tile_update */
} FigStreamIOTileBinding;

/** Zeroes `slots` (grid->cfg.slots_x * grid->cfg.slots_y entries). Call
 *  once at boot, before fig_tile_init. `grid` must be the SAME grid the
 *  manager is later initialised with — the slot indexing has to agree. */
void fig_streamio_tile_bind(FigStreamIOTileBinding *b, FigStreamIO *io,
                             FigTileGrid *grid, FigStreamIOSlot *slots,
                             FigStreamIOAssetType type,
                             FigStreamIOTileKeyFn key_fn);

/** Pass as FigTileLoadFn/UnloadFn to fig_tile_init with
 *  user_ctx = &binding. REQUIRES the manager's load_budget to be set to
 *  255 (see the file comment) — fig_tile_init defaults to 2, which must
 *  be overridden after init, before the first fig_tile_update call. */
void *fig_streamio_tile_on_load  (int16_t tx, int16_t ty, uint8_t lod, void *user);
void  fig_streamio_tile_on_unload(int16_t tx, int16_t ty, uint8_t lod,
                                   void *user_data, void *user);

#ifdef __cplusplus
}
#endif

#endif /* FIG_STREAMIO_H */
